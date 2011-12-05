/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "config.h"

#include <sys/time.h>
#include <sys/resource.h>

#include <errno.h>
#include <stdarg.h>
#include <string.h>

#include <sstream>
#include <fstream>

#include "elliptics/cppdef.h"

using namespace zbr;

static void test_log_raw(elliptics_log *l, uint32_t mask, const char *format, ...)
{
	va_list args;
	char buf[1024];
	int buflen = sizeof(buf);

	if (!(l->get_log_mask() & mask))
		return;

	va_start(args, format);
	vsnprintf(buf, buflen, format, args);
	buf[buflen-1] = '\0';
	l->log(mask, buf);
	va_end(args);
}

class elliptics_callback_io : public elliptics_callback {
	public:
		elliptics_callback_io(elliptics_log *l) { log = l; };
		virtual ~elliptics_callback_io() {};

		virtual int		callback(struct dnet_net_state *state, struct dnet_cmd *cmd, struct dnet_attr *attr);

	private:
		elliptics_log		*log;
};

int elliptics_callback_io::callback(struct dnet_net_state *state, struct dnet_cmd *cmd, struct dnet_attr *attr)
{
	int err;
	struct dnet_io_attr *io;

	if (is_trans_destroyed(state, cmd, attr)) {
		err = -EINVAL;
		goto err_out_exit;
	}

	if (cmd->status || !cmd->size) {
		err = cmd->status;
		goto err_out_exit;
	}

	if (cmd->size <= sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr)) {
		test_log_raw(log, DNET_LOG_ERROR, "%s: read completion error: wrong size: "
				"cmd_size: %llu, must be more than %zu.\n",
				dnet_dump_id(&cmd->id), (unsigned long long)cmd->size,
				sizeof(struct dnet_attr) + sizeof(struct dnet_io_attr));
		err = -EINVAL;
		goto err_out_exit;
	}

	if (!attr) {
		test_log_raw(log, DNET_LOG_ERROR, "%s: no attributes but command size is not null.\n",
				dnet_dump_id(&cmd->id));
		err = -EINVAL;
		goto err_out_exit;
	}

	io = (struct dnet_io_attr *)(attr + 1);

	dnet_convert_io_attr(io);
	err = 0;

	test_log_raw(log, DNET_LOG_INFO, "%s: io completion: offset: %llu, size: %llu.\n",
			dnet_dump_id(&cmd->id), (unsigned long long)io->offset, (unsigned long long)io->size);

err_out_exit:
	if (!cmd || !(cmd->flags & DNET_FLAGS_MORE))
		test_log_raw(log, DNET_LOG_INFO, "%s: io completed: %d.\n", cmd ? dnet_dump_id(&cmd->id) : "nil", err);
	return err;
}

static void test_prepare_commit(elliptics_node &n, int psize, int csize)
{
	std::string written, ret;
	try {
		std::string key = "prepare-commit-test";

		std::string prepare_data = "prepare data|";
		std::string commit_data = "commit data";
		std::string plain_data[3] = {"plain data0|", "plain data1|", "plain data2|"};

		if (psize)
			prepare_data.clear();
		if (csize)
			commit_data.clear();

		uint64_t offset = 0;
		uint64_t total_size_to_reserve = 1024;

		unsigned int aflags = 0;
		unsigned int ioflags = 0;

		int column = 0;

		n.write_prepare(key, prepare_data, offset, total_size_to_reserve, aflags, ioflags, column);
		offset += prepare_data.size();

		written += prepare_data;

		for (int i = 0; i < 3; ++i) {
			n.write_plain(key, plain_data[i], offset, aflags, ioflags, column);
			offset += plain_data[i].size();

			written += plain_data[i];
		}

		n.write_commit(key, commit_data, offset, 0, aflags, ioflags, column);
		written += commit_data;

		ret = n.read_data_wait(key, 0, 0, aflags, ioflags, column);
		std::cout << "prepare/commit write: '" << written << "', read: '" << ret << "'" << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "PREPARE/COMMIT test failed: " << e.what() << std::endl;
		throw;
	}

	if (ret != written) {
		std::cerr << "PREPARE/COMMIT test failed: read mismatch" << std::endl;
		throw std::runtime_error("PREPARE/COMMIT test failed: read mismatch");
	}
}

static void test_range_request(elliptics_node &n, int limit_start, int limit_num, unsigned int aflags)
{
	struct dnet_io_attr io;
	char id_str[DNET_ID_SIZE * 2 + 1];

	memset(&io, 0, sizeof(io));

#if 0
	dnet_parse_numeric_id("76a046fcd25ebeaaa65a0fa692faf8b8701695c6ba67008b5922ae9f134fc1da7ffffed191edf767000000000000000000000000000000000000000000000000", &io.id);
	dnet_parse_numeric_id("76a046fcd25ebeaaa65a0fa692faf8b8701695c6ba67008b5922ae9f134fc1da7ffffed22220037fffffffffffffffffffffffffffffffffffffffffffffffff", &io.parent);
#else
	memset(io.id, 0x00, sizeof(io.id));
	memset(io.parent, 0xff, sizeof(io.id));
#endif
	io.start = limit_start;
	io.num = limit_num;

	int group_id = 2;

	std::vector<std::string> ret;
	ret = n.read_data_range(io, group_id, aflags);

	std::cout << "range [LIMIT(" << limit_start << ", " << limit_num << "): " << ret.size() << " elements" << std::endl;
	for (size_t i = 0; i < ret.size(); ++i) {
		const char *data = ret[i].data();
		const unsigned char *id = (const unsigned char *)data;
		uint64_t size = dnet_bswap64(*(uint64_t *)(data + DNET_ID_SIZE));
		char *str = (char *)(data + DNET_ID_SIZE + 8);

#if 0
		std::cout << "range [LIMIT(" << limit_start << ", " << limit_num << "): " <<
			dnet_dump_id_len_raw(id, DNET_ID_SIZE, id_str) << ": size: " << size << ": " << str << std::endl;
#endif
	}
}

static void test_lookup_parse(const std::string &key, const std::string &lret)
{
	struct dnet_addr *addr = (struct dnet_addr *)lret.data();
	struct dnet_cmd *cmd = (struct dnet_cmd *)(addr + 1);
	struct dnet_attr *attr = (struct dnet_attr *)(cmd + 1);
	struct dnet_addr_attr *a = (struct dnet_addr_attr *)(attr + 1);

	dnet_convert_addr_attr(a);
	std::cout << key << ": lives on addr: " << dnet_server_convert_dnet_addr(&a->addr);

	if (attr->size > sizeof(struct dnet_addr_attr)) {
		struct dnet_file_info *info = (struct dnet_file_info *)(a + 1);

		dnet_convert_file_info(info);
		std::cout << ": mode: " << std::oct << info->mode << std::dec;
		std::cout << ", offset: " << (unsigned long long)info->offset;
		std::cout << ", size: " << (unsigned long long)info->size;
		std::cout << ", file: " << (char *)(info + 1);
	}
	std::cout << std::endl;
}

static void test_lookup(elliptics_node &n, std::vector<int> &groups)
{
	try {
		std::string key = "2.xml";
		std::string data = "lookup data";

		std::string lret = n.write_data_wait(key, data, 0, 0, 0, 0);
		test_lookup_parse(key, lret);

		struct dnet_id id;
		n.transform(key, id);
		id.group_id = 0;
		id.type = 0;

		int aflags = 0;

		struct timespec ts = {0, 0};
		n.write_metadata(id, key, groups, ts, aflags);

		lret = n.lookup(key);
		test_lookup_parse(key, lret);
	} catch (const std::exception &e) {
		std::cerr << "LOOKUP test failed: " << e.what() << std::endl;
	}
}

static void test_append(elliptics_node &n)
{
	try {
		std::string key = "append-test";
		std::string data = "first part of the message";

		n.write_data_wait(key, data, 0, 0, 0, 0);

		data = "| second part of the message";
		n.write_data_wait(key, data, 0, 0, DNET_IO_FLAGS_APPEND, 0);

		std::cout << key << ": " << n.read_data_wait(key, 0, 0, 0, 0, 0) << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "APPEND test failed: " << e.what() << std::endl;
	}
}

static void test_exec_python(elliptics_node &n)
{
	try {
		std::string binary = "binary data";
		std::string script = "from time import time, ctime\n"
			"__return_data = 'Current time is ' + ctime(time()) + '"
				"|received binary data: ' + __input_binary_data_tuple[0].decode('utf-8')";

		std::string ret;

		ret = n.exec(NULL, script, binary, DNET_EXEC_PYTHON);

		std::cout << "sent script: " << ret << std::endl;

		script = "local_addr_string = 'this is local addr string' + "
				"'|received binary data: ' + __input_binary_data_tuple[0].decode('utf-8')";
		std::string name = "test_script.py";

		std::cout << "remote script: " << name << ": " <<
			n.exec_name(NULL, name, script, binary, DNET_EXEC_PYTHON_SCRIPT_NAME) << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "PYTHON exec test failed: " << e.what() << std::endl;
	}
}

static void read_column_raw(elliptics_node &n, const std::string &key, const std::string &data, int column)
{
	std::string ret;
	try {
		ret = n.read_data_wait(key, 0, 0, 0, 0, column);
	} catch (const std::exception &e) {
		std::cerr << "COLUMN-" << column << " read test failed: " << e.what() << std::endl;
		throw;
	}

	std::cout << "read-column-" << column << ": " << key << " : " << ret << std::endl;
	if (ret != data) {
		throw std::runtime_error("column test failed");
	}
}

static void column_test(elliptics_node &n)
{
	std::string key = "some-key-1";

	std::string data0 = "some-compressed-data-in-column-0";
	std::string data1 = "some-data-in-column-2";
	std::string data2 = "some-data-in-column-3";

	n.write_data_wait(key, data0, 0, 0, DNET_IO_FLAGS_COMPRESS, 0);
	n.write_data_wait(key, data1, 0, 0, 0, 2);
	n.write_data_wait(key, data2, 0, 0, 0, 3);

	read_column_raw(n, key, data0, 0);
	read_column_raw(n, key, data1, 2);
	read_column_raw(n, key, data2, 3);
}

static void test_bulk_write(elliptics_node &n)
{
	try {
		std::vector<struct dnet_io_attr> ios;
		std::vector<std::string> data;

		int i;

		for (i = 0; i < 3; ++i) {
			std::ostringstream os;
			struct dnet_io_attr io;
			struct dnet_id id;

			os << "bulk_write" << i;

			memset(&io, 0, sizeof(io));

			n.transform(os.str(), id);
			memcpy(io.id, id.id, DNET_ID_SIZE);
			io.type = id.type;
			io.size = os.str().size();

			ios.push_back(io);
			data.push_back(os.str());
		}

		std::string ret = n.bulk_write(ios, data, 0);

		std::cout << "ret size = " << ret.size() << std::endl;

		/* read without checksums since we did not write metadata */
		for (i = 0; i < 3; ++i) {
			std::ostringstream os;

			os << "bulk_write" << i;
			std::cout << os.str() << ": " << n.read_data_wait(os.str(), 0, 0, DNET_ATTR_NOCSUM, 0, 0) << std::endl;
		}
	} catch (const std::exception &e) {
		std::cerr << "BULK WRITE test failed: " << e.what() << std::endl;
	}
}

static void test_bulk_read(elliptics_node &n)
{
	try {
		std::vector<std::string> keys;

		int i;

		for (i = 0; i < 3; ++i) {
			std::ostringstream os;
			os << "bulk_write" << i;
			keys.push_back(os.str());
		}

		int group_id = 2;
		std::vector<std::string> ret = n.bulk_read(keys, group_id, 0);

		std::cout << "ret size = " << ret.size() << std::endl;

		/* read without checksums since we did not write metadata */
		for (i = 0; i < 3; ++i) {
			std::ostringstream os;

			os << "bulk_read" << i;
			std::cout << os.str() << ": " << ret[i].substr(DNET_ID_SIZE + 8) << std::endl;
		}
	} catch (const std::exception &e) {
		std::cerr << "BULK READ test failed: " << e.what() << std::endl;
	}

static void memory_test_io(elliptics_node &n, int num)
{
	int ids[16];

	for (int i = 0; i < num; ++i) {
		std::string data;

		data.resize(rand() % 102400 + 100);

		for (int j = 0; j < (int)ARRAY_SIZE(ids); ++j)
			ids[j] = rand();

		std::string id((char *)ids, sizeof(ids));
		std::string written;

		try {
			written = n.write_data_wait(id, data, 0, 0, 0, 0);
			std::string res = n.read_data_wait(id, 0, 0, 0, 0, 0);
		} catch (const std::exception &e) {
			std::cerr << "could not perform read/write: " << e.what() << std::endl;
			if (written.size() > 0) {
				std::cerr << "but written successfully\n";
				test_lookup_parse(id, written);
			}
		}
	}

}

static void memory_test_script(elliptics_node &n, int num)
{
	int ids[16];

	for (int i = 0; i < num; ++i) {
		std::string data;

		data.resize(rand() % 102400 + 100);

		for (int j = 0; j < (int)ARRAY_SIZE(ids); ++j)
			ids[j] = rand();

		std::string id((char *)ids, sizeof(ids));
		id.resize(DNET_ID_SIZE);
		id.append(data);

		std::string written;

		try {
			std::string script = "binary = str(__input_binary_data_tuple[0])\n"
				"n.write_data(binary[0:64], binary[64:], 0, 0, 0, 0)\n"
				"__return_data = n.read_data(binary[0:64], 0, 0, 0, 0, 0)";

			written = n.exec(NULL, script, id, DNET_EXEC_PYTHON);
		} catch (const std::exception &e) {
			std::cerr << "could not perform read/write: " << e.what() << std::endl;
			if (written.size() > 0) {
				std::cerr << "but written successfully\n";
				test_lookup_parse(id, written);
			}
		}
	}

}

static void memory_test(elliptics_node &n)
{
	struct rusage start, end;

	getrusage(RUSAGE_SELF, &start);
	memory_test_script(n, 1000);
	getrusage(RUSAGE_SELF, &end);

	std::cout << "script leaked: " << end.ru_maxrss - start.ru_maxrss << " Kb\n";

	getrusage(RUSAGE_SELF, &start);
	memory_test_io(n, 1000);
	getrusage(RUSAGE_SELF, &end);

	std::cout << "IO leaked: " << end.ru_maxrss - start.ru_maxrss << " Kb\n";
}

int main(int argc, char *argv[])
{
	int g[] = {1, 2, 3};
	std::vector<int> groups(g, g+ARRAY_SIZE(g));
	char *host = (char *)"localhost";

	if (argc > 1)
		host = argv[1];

	try {
		elliptics_log_file log("/dev/stderr", DNET_LOG_ERROR | DNET_LOG_DATA);

		elliptics_node n(log);
		n.add_groups(groups);

		int ports[] = {1025, 1026};
		int added = 0;

		for (int i = 0; i < (int)ARRAY_SIZE(ports); ++i) {
			try {
				n.add_remote(host, ports[i], AF_INET);
				added++;
			} catch (...) {
			}
		}

		if (!added)
			throw std::runtime_error("Could not add remote nodes, exiting");

		test_lookup(n, groups);

		n.stat_log();

		column_test(n);

		test_prepare_commit(n, 0, 0);
		test_prepare_commit(n, 1, 0);
		test_prepare_commit(n, 0, 1);
		test_prepare_commit(n, 1, 1);

		test_range_request(n, 0, 0, 0);
		test_range_request(n, 0, 0, DNET_ATTR_SORT);
		test_range_request(n, 1, 0, 0);
		test_range_request(n, 0, 1, 0);

		test_append(n);

		test_exec_python(n);

		test_bulk_write(n);
		test_bulk_read(n);

		memory_test(n);
	} catch (const std::exception &e) {
		std::cerr << "Error occured : " << e.what() << std::endl;
	} catch (int err) {
		std::cerr << "Error : " << err << std::endl;
	}
}
