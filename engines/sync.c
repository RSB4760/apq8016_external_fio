/*
 * sync/psync engine
 *
 * IO engine that does regular read(2)/write(2) with lseek(2) to transfer
 * data and IO engine that does regular pread(2)/pwrite(2) to transfer data.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "../fio.h"

struct syncio_data {
	struct iovec *iovecs;
	struct io_u **io_us;
	unsigned int queued;
	unsigned long queued_bytes;

	unsigned long long last_offset;
	struct fio_file *last_file;
	enum fio_ddir last_ddir;
};

static int fio_syncio_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;

	if (io_u->ddir == DDIR_SYNC)
		return 0;
	if (io_u->offset == f->last_completed_pos)
		return 0;

	if (lseek(f->fd, io_u->offset, SEEK_SET) == -1) {
		td_verror(td, errno, "lseek");
		return 1;
	}

	return 0;
}

static int fio_io_end(struct thread_data *td, struct io_u *io_u, int ret)
{
	if (ret != (int) io_u->xfer_buflen) {
		if (ret >= 0) {
			io_u->resid = io_u->xfer_buflen - ret;
			io_u->error = 0;
			return FIO_Q_COMPLETED;
		} else
			io_u->error = errno;
	}

	if (io_u->error)
		td_verror(td, io_u->error, "xfer");

	return FIO_Q_COMPLETED;
}

static int fio_psyncio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		ret = pread(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else if (io_u->ddir == DDIR_WRITE)
		ret = pwrite(f->fd, io_u->xfer_buf, io_u->xfer_buflen, io_u->offset);
	else
		ret = fsync(f->fd);

	return fio_io_end(td, io_u, ret);
}

static int fio_syncio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	int ret;

	fio_ro_check(td, io_u);

	if (io_u->ddir == DDIR_READ)
		ret = read(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else if (io_u->ddir == DDIR_WRITE)
		ret = write(f->fd, io_u->xfer_buf, io_u->xfer_buflen);
	else
		ret = fsync(f->fd);

	return fio_io_end(td, io_u, ret);
}

static int fio_vsyncio_getevents(struct thread_data *td, unsigned int min,
				 unsigned int max,
				 struct timespec fio_unused *t)
{
	struct syncio_data *sd = td->io_ops->data;
	int ret;

	if (min) {
		ret = sd->queued;
		sd->queued = 0;
	} else
		ret = 0;

	dprint(FD_IO, "vsyncio_getevents: min=%d,max=%d: %d\n", min, max, ret);
	return ret;
}

static struct io_u *fio_vsyncio_event(struct thread_data *td, int event)
{
	struct syncio_data *sd = td->io_ops->data;

	return sd->io_us[event];
}

static int fio_vsyncio_append(struct thread_data *td, struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops->data;

	if (io_u->ddir == DDIR_SYNC)
		return 0;

	if (io_u->offset == sd->last_offset && io_u->file == sd->last_file &&
	    io_u->ddir == sd->last_ddir)
		return 1;

	return 0;
}

static void fio_vsyncio_set_iov(struct syncio_data *sd, struct io_u *io_u,
				int index)
{
	sd->io_us[index] = io_u;
	sd->iovecs[index].iov_base = io_u->xfer_buf;
	sd->iovecs[index].iov_len = io_u->xfer_buflen;
	sd->last_offset = io_u->offset + io_u->xfer_buflen;
	sd->last_file = io_u->file;
	sd->last_ddir = io_u->ddir;
	sd->queued_bytes += io_u->xfer_buflen;
	sd->queued++;
}

static int fio_vsyncio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct syncio_data *sd = td->io_ops->data;

	fio_ro_check(td, io_u);

	if (!fio_vsyncio_append(td, io_u)) {
		dprint(FD_IO, "vsyncio_queue: no append (%d)\n", sd->queued);
		/*
		 * If we can't append and have stuff queued, tell fio to
		 * commit those first and then retry this io
		 */
		if (sd->queued)
			return FIO_Q_BUSY;
		if (io_u->ddir == DDIR_SYNC) {
			int ret = fsync(io_u->file->fd);

			return fio_io_end(td, io_u, ret);
		}

		sd->queued = 0;
		sd->queued_bytes = 0;
		fio_vsyncio_set_iov(sd, io_u, 0);
	} else {
		if (sd->queued == td->o.iodepth) {
			dprint(FD_IO, "vsyncio_queue: max depth %d\n", sd->queued);
			return FIO_Q_BUSY;
		}

		dprint(FD_IO, "vsyncio_queue: append\n");
		fio_vsyncio_set_iov(sd, io_u, sd->queued);
	}

	dprint(FD_IO, "vsyncio_queue: depth now %d\n", sd->queued);
	return FIO_Q_QUEUED;
}

/*
 * Check that we transferred all bytes, or saw an error, etc
 */
static int fio_vsyncio_end(struct thread_data *td, ssize_t bytes)
{
	struct syncio_data *sd = td->io_ops->data;
	struct io_u *io_u;
	unsigned int i;
	int err;

	/*
	 * transferred everything, perfect
	 */
	if (bytes == sd->queued_bytes)
		return 0;

	err = errno;
	for (i = 0; i < sd->queued; i++) {
		io_u = sd->io_us[i];

		if (bytes == -1) {
			io_u->error = err;
		} else {
			unsigned int this_io;

			this_io = bytes;
			if (this_io > io_u->xfer_buflen)
				this_io = io_u->xfer_buflen;

			io_u->resid = io_u->xfer_buflen - this_io;
			io_u->error = 0;
			bytes -= this_io;
		}
	}

	if (bytes == -1) {
		td_verror(td, err, "xfer vsync");
		return -err;
	}

	return 0;
}

static int fio_vsyncio_commit(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops->data;
	struct fio_file *f;
	ssize_t ret;

	if (!sd->queued)
		return 0;

	f = sd->last_file;

	if (lseek(f->fd, sd->io_us[0]->offset, SEEK_SET) == -1) {
		int err = -errno;

		td_verror(td, errno, "lseek");
		return err;
	}

	if (sd->last_ddir == DDIR_READ)
		ret = readv(f->fd, sd->iovecs, sd->queued);
	else
		ret = writev(f->fd, sd->iovecs, sd->queued);

	dprint(FD_IO, "vsyncio_commit: %d\n", (int) ret);
	return fio_vsyncio_end(td, ret);
}

static int fio_vsyncio_init(struct thread_data *td)
{
	struct syncio_data *sd;

	sd = malloc(sizeof(*sd));
	memset(sd, 0, sizeof(*sd));
	sd->last_offset = -1ULL;
	sd->iovecs = malloc(td->o.iodepth * sizeof(struct iovec));
	sd->io_us = malloc(td->o.iodepth * sizeof(struct io_u *));

	td->io_ops->data = sd;
	return 0;
}

static void fio_vsyncio_cleanup(struct thread_data *td)
{
	struct syncio_data *sd = td->io_ops->data;

	free(sd->iovecs);
	free(sd->io_us);
	free(sd);
	td->io_ops->data = NULL;
}

static struct ioengine_ops ioengine_rw = {
	.name		= "sync",
	.version	= FIO_IOOPS_VERSION,
	.prep		= fio_syncio_prep,
	.queue		= fio_syncio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.flags		= FIO_SYNCIO,
};

static struct ioengine_ops ioengine_prw = {
	.name		= "psync",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_psyncio_queue,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.flags		= FIO_SYNCIO,
};

static struct ioengine_ops ioengine_vrw = {
	.name		= "vsync",
	.version	= FIO_IOOPS_VERSION,
	.init		= fio_vsyncio_init,
	.cleanup	= fio_vsyncio_cleanup,
	.queue		= fio_vsyncio_queue,
	.commit		= fio_vsyncio_commit,
	.event		= fio_vsyncio_event,
	.getevents	= fio_vsyncio_getevents,
	.open_file	= generic_open_file,
	.close_file	= generic_close_file,
	.flags		= FIO_SYNCIO,
};

static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine_rw);
	register_ioengine(&ioengine_prw);
	register_ioengine(&ioengine_vrw);
}

static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine_rw);
	unregister_ioengine(&ioengine_prw);
	unregister_ioengine(&ioengine_vrw);
}
