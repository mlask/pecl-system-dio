/*
   +----------------------------------------------------------------------+
   | PHP Version 7                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 2009 Melanie Rhianna Lewis                             |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.0 of the PHP license,       |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt.                                 |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Melanie Rhianna Lewis <cyberspice@php.net>                   |
   +----------------------------------------------------------------------+
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/url.h"

#include "php_dio.h"
#include "php_dio_common.h"
#include "php_dio_stream_wrappers.h"

static int d_stream;
#define D_STREAM "DIO Stream"
#if PHP_MAJOR_VERSION >= 7
    #define ZEND_FETCH_RESOURCE(rsrc, rsrc_type, passed_id, default_id, resource_type_name, resource_type) \
        (rsrc = (rsrc_type) zend_fetch_resource(Z_RES_P(*passed_id), resource_type_name, resource_type))
#endif

/*
   +----------------------------------------------------------------------+
   | Raw stream handling                                                  |
   +----------------------------------------------------------------------+
*/

/* {{{ dio_stream_write
 * Write to the stream
 */
static size_t dio_stream_write(php_stream *stream, const char *buf, size_t count)
{
	return dio_common_write((php_dio_stream_data*)stream->abstract, buf, count);
}
/* }}} */

/* {{{ dio_stream_read
 * Read from the stream
 */
static size_t dio_stream_read(php_stream *stream, char *buf, size_t count)
{
	php_dio_stream_data* data = (php_dio_stream_data*)stream->abstract;
	size_t bytes = dio_common_read(data, buf, count);
	stream->eof = data->end_of_file;

	return bytes;
}
/* }}} */

/* {{{ dio_stream_flush
 * Flush the stream.  For raw streams this does nothing.
 */
static int dio_stream_flush(php_stream *stream)
{
	return 1;
}
/* }}} */

/* {{{ dio_stream_close
 * Close the stream
 */
static int dio_stream_close(php_stream *stream, int close_handle)
{
	php_dio_stream_data *abstract = (php_dio_stream_data*)stream->abstract;

	if (!dio_common_close(abstract)) {
		return 0;
	}

	efree(abstract);
	return 1;
}
/* }}} */

/* {{{ dio_stream_set_option
 * Set the stream options.
 */
static int dio_stream_set_option(php_stream *stream, int option, int value, void *ptrparam)
{
	php_dio_stream_data *abstract = (php_dio_stream_data*)stream->abstract;

	switch (option) {
		case PHP_STREAM_OPTION_META_DATA_API:
#ifdef DIO_NONBLOCK
			add_assoc_bool((zval *)ptrparam, "timed_out", abstract->timed_out);
			add_assoc_bool((zval *)ptrparam, "blocked", abstract->is_blocking);
#endif
			add_assoc_bool((zval *)ptrparam, "eof", stream->eof);
			return PHP_STREAM_OPTION_RETURN_OK;

#if PHP_MAJOR_VERSION >= 5
		case PHP_STREAM_OPTION_CHECK_LIVENESS:
			stream->eof = abstract->end_of_file;
			return PHP_STREAM_OPTION_RETURN_OK;
#endif /* PHP_MAJOR_VERSION >= 5 */

		default:
			break;
	}

	return dio_common_set_option(abstract, option, value, ptrparam);
}
/* }}} */

php_stream_ops dio_raw_stream_ops = {
	dio_stream_write,
	dio_stream_read,
	dio_stream_close,
	dio_stream_flush,
	"dio",
	NULL, /* seek */
	NULL, /* cast */
	NULL, /* stat */
	dio_stream_set_option,
};

/* {{{ dio_raw_fopen_wrapper
 * fopen for the dio.raw stream.
 */
static php_stream *dio_raw_fopen_wrapper(php_stream_wrapper *wrapper,
                                         const char *path, const char *mode,
                                         int options, zend_string **opened_path,
                                         php_stream_context *context STREAMS_DC) {
	php_dio_stream_data *data;
	php_stream *stream;
	const char *filename;

	/* Check it was actually for us (not a corrupted function pointer
	   somewhere!). */
	if (strncmp(path, DIO_RAW_STREAM_PROTOCOL, sizeof(DIO_RAW_STREAM_PROTOCOL) - 1)) {
		return NULL;
	}

	/* Get the actually file system name/path. */
	filename = path + sizeof(DIO_RAW_STREAM_PROTOCOL) - 1;

	/* Check we can actually access it. */
	if (php_check_open_basedir(filename) || DIO_SAFE_MODE_CHECK(filename, mode)) {
		return NULL;
	}

	data = dio_create_stream_data();
	data->stream_type = DIO_STREAM_TYPE_RAW;

	/* Parse the context. */
	if (context) {
		dio_stream_context_get_basic_options(context, data);
	}

	/* Try and open a raw stream. */
	if (!dio_raw_open_stream(filename, mode, data)) {
		return NULL;
	}

	/* Create a PHP stream based on raw stream */
	stream = php_stream_alloc(&dio_raw_stream_ops, data, 0, mode);
	if (!stream) {
		(void) dio_common_close(data);
		efree(data);
	}

	return stream;
}
/* }}} */

static php_stream_wrapper_ops dio_raw_stream_wops = {
	dio_raw_fopen_wrapper,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	DIO_RAW_STREAM_NAME
};

php_stream_wrapper php_dio_raw_stream_wrapper = {
	&dio_raw_stream_wops,
	NULL,
	0
};

/* {{{ proto dio_raw(string filename, string mode[, array options])
 * Opens a raw direct IO stream.
 */
PHP_FUNCTION(dio_raw) {
	zval *options = NULL;
	php_dio_stream_data *data;
	php_stream *stream;

	char *filename;
	size_t filename_len;
	char *mode;
	size_t mode_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss|z", &filename, &filename_len, &mode, &mode_len, &options) == FAILURE) {
		RETURN_FALSE;
	}

	/* Check the third argument is an array. */
	if (options && (Z_TYPE_P(options) != IS_ARRAY)) {
		RETURN_FALSE;
	}

	/* Check we can actually access the file. */
	if (php_check_open_basedir(filename) || DIO_SAFE_MODE_CHECK(filename, mode)) {
		RETURN_FALSE;
	}

	data = dio_create_stream_data();
	data->stream_type = DIO_STREAM_TYPE_RAW;

	if (options) {
		dio_assoc_array_get_basic_options(options, data);
	}

	/* Try and open a raw stream. */
	if (dio_raw_open_stream(filename, mode, data)) {
		stream = php_stream_alloc(&dio_raw_stream_ops, data, 0, mode);
		if (!stream) {
			(void) dio_common_close(data);
			efree(data);
			RETURN_FALSE;
		}
		php_stream_to_zval(stream, return_value);
	}
}
/* }}} */

/*
   +----------------------------------------------------------------------+
   | Serial stream handling                                               |
   +----------------------------------------------------------------------+
*/

/* {{{ dio_stream_flush
 * Flush the stream.  If the stream is read only, it flushes the read
 * stream, if it is write only it flushes the write, otherwise it flushes
 * both.
 */
static int dio_serial_stream_flush(php_stream *stream)
{
	return dio_serial_purge((php_dio_stream_data*)stream->abstract);
}
/* }}} */

/* {{{ dio_stream_close
 * Close the stream.  Restores the serial settings to their value before
 * the stream was open.
 */
static int dio_serial_stream_close(php_stream *stream, int close_handle)
{
	php_dio_stream_data *abstract = (php_dio_stream_data*)stream->abstract;

	if (!dio_serial_uninit(abstract)) {
		return 0;
	}

	if (!dio_common_close(abstract)) {
		return 0;
	}

	efree(abstract);
	return 1;
}
/* }}} */

static int dio_stream_cast(php_stream *stream, int cast_as, void **ret)
{
	php_dio_stream_data *data = (php_dio_stream_data*)stream->abstract;
	php_dio_posix_stream_data *pdata = (php_dio_posix_stream_data*)data;

	switch (cast_as)	{
		case PHP_STREAM_AS_FD_FOR_SELECT:
		case PHP_STREAM_AS_FD:
			if (ret) {
				*(int *)ret = pdata->fd;
			}
			return SUCCESS;
		default:
			return FAILURE;
	}
}

php_stream_ops dio_serial_stream_ops = {
	dio_stream_write,
	dio_stream_read,
	dio_serial_stream_close,
	dio_serial_stream_flush,
	"dio",
	NULL, /* seek */
	dio_stream_cast,//NULL, /* cast */
	NULL, /* stat */
	dio_stream_set_option,
};

/* {{{ dio_raw_fopen_wrapper
 * fopen for the dio.raw stream.
 */
static php_stream *dio_serial_fopen_wrapper(php_stream_wrapper *wrapper,
                                         const char *path, const char *mode,
                                         int options, zend_string **opened_path,
                                         php_stream_context *context STREAMS_DC) {
	php_dio_stream_data *data;
	php_stream *stream;
	const char *filename;

	/* Check it was actually for us (not a corrupted function pointer
	   somewhere!). */
	if (strncmp(path, DIO_SERIAL_STREAM_PROTOCOL, sizeof(DIO_SERIAL_STREAM_PROTOCOL) - 1)) {
		return NULL;
	}

	/* Get the actually file system name/path. */
	filename = path + sizeof(DIO_SERIAL_STREAM_PROTOCOL) - 1;

	/* Check we can actually access it. */
	if (php_check_open_basedir(filename) || DIO_SAFE_MODE_CHECK(filename, mode)) {
		return NULL;
	}

	data = dio_create_stream_data();
	data->stream_type = DIO_STREAM_TYPE_SERIAL;

	/* Parse the context. */
	if (context) {
		dio_stream_context_get_basic_options(context, data);
		dio_stream_context_get_serial_options(context, data);
	}

	/* Try and open a serial stream. */
	if (!dio_serial_open_stream(filename, mode, data)) {
		return NULL;
	}

	stream = php_stream_alloc(&dio_serial_stream_ops, data, 0, mode);
	if (!stream) {
		efree(data);
	}

	return stream;
}
/* }}} */

static php_stream_wrapper_ops dio_serial_stream_wops = {
	dio_serial_fopen_wrapper,
	NULL, /* stream_close */
	NULL, /* stat */
	NULL, /* stat_url */
	NULL, /* opendir */
	DIO_SERIAL_STREAM_NAME
};

php_stream_wrapper php_dio_serial_stream_wrapper = {
	&dio_serial_stream_wops,
	NULL,
	0
};

/* {{{ proto dio_serial(string filename, string mode[, array options])
 * Opens a serial direct IO stream.
 */
PHP_FUNCTION(dio_serial) {
	zval *options = NULL;
	php_dio_stream_data *data;
	php_stream *stream;

	char *filename;
	size_t filename_len;
	char *mode;
	size_t mode_len;
printf("dio_serial\n");

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss|z", &filename, &filename_len, &mode, &mode_len, &options) == FAILURE) {
		RETURN_FALSE;
	}

	/* Check the third argument is an array. */
	if (options && (Z_TYPE_P(options) != IS_ARRAY)) {
		php_error_docref(NULL, E_WARNING,"dio_serial, the third argument should be an array of options");
		RETURN_FALSE;
	}

	/* Check we can actually access the file. */
	if (php_check_open_basedir(filename) || DIO_SAFE_MODE_CHECK(filename, mode)) {
		RETURN_FALSE;
	}

	data = dio_create_stream_data();
	data->stream_type = DIO_STREAM_TYPE_SERIAL;

	if (options) {
		dio_assoc_array_get_basic_options(options, data);
		dio_assoc_array_get_serial_options(options, data);
	}

	/* Try and open a serial stream. */
	if (dio_serial_open_stream(filename, mode, data)) {
		stream = php_stream_alloc(&dio_serial_stream_ops, data, 0, mode);
		if (!stream) {
			efree(data);
			RETURN_FALSE;
		}
		php_stream_to_zval(stream, return_value);
	}
}
/* }}} */

static int dio_data_rate_to_define(long rate, speed_t *def) {
	speed_t val;

	switch (rate) {
		case 0:
			val = 0;
			break;
		case 50:
			val = B50;
			break;
		case 75:
			val = B75;
			break;
		case 110:
			val = B110;
			break;
		case 134:
			val = B134;
			break;
		case 150:
			val = B150;
			break;
		case 200:
			val = B200;
			break;
		case 300:
			val = B300;
			break;
		case 600:
			val = B600;
			break;
		case 1200:
			val = B1200;
			break;
		case 1800:
			val = B1800;
			break;
		case 2400:
			val = B2400;
			break;
		case 4800:
			val = B4800;
			break;
		case 9600:
			val = B9600;
			break;
		case 19200:
			val = B19200;
			break;
		case 38400:
			val = B38400;
			break;
#ifdef B57600
		case 57600:
			val = B57600;
			break;
#endif
#ifdef B115200
		case 115200:
			val = B115200;
			break;
#endif
#ifdef B230400
		case 230400:
			val = B230400;
			break;
#endif
#ifdef B460800
		case 460800:
			val = B460800;
			break;
#endif
		default:
			return 0;
	}

	*def = val;
	return 1;
}

/* {{{ proto stream_set_baudrate(resource stream, int baudrate)
 * Sets stream baudrate in runtime.
 */
PHP_FUNCTION(stream_set_baudrate) {
	zval *zstream;
	zend_long baudrate;
	php_stream *stream;
	struct termios tio;
	speed_t rate_def;
	int ret = 0;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_RESOURCE(zstream)
		Z_PARAM_LONG(baudrate)
	ZEND_PARSE_PARAMETERS_END();

	php_stream_from_zval(stream, zstream);

	if (!dio_data_rate_to_define(baudrate, &rate_def)) {
		php_error_docref(NULL, E_WARNING, "invalid data_rate value (%ld)", baudrate);
		RETURN_FALSE;
	}

	php_dio_stream_data *data = (php_dio_stream_data*)stream->abstract;
	php_dio_posix_stream_data *pdata = (php_dio_posix_stream_data*)data;
	ret = tcgetattr(pdata->fd, &tio);
	if (ret < 0) {
		RETURN_FALSE;
	}

	cfsetispeed(&tio, rate_def);
	cfsetospeed(&tio, rate_def);

	ret = tcsetattr(pdata->fd, TCSANOW, &tio);
	if (ret < 0) {
		RETURN_FALSE;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto stream_select_timeout(long timeout_sec, long timeout_usec)
 * Perform stream_select without file descriptors.
 */
PHP_FUNCTION(stream_select_timeout) {
	zend_long timeout_sec, timeout_usec;
	struct timeval timeout;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_LONG(timeout_sec)
		Z_PARAM_LONG(timeout_usec)
	ZEND_PARSE_PARAMETERS_END();

	timeout.tv_sec  = timeout_sec;
	timeout.tv_usec = timeout_usec;
	select(0, NULL, NULL, NULL, &timeout);
}
/* }}} */

/*
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 4
 * End:
 * vim600: fdm=marker
 * vim: sw=4 ts=4 noet
 */
