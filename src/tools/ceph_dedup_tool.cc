// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2018 Myoungwon Oh <ohmyoungwon@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#include "include/types.h"

#include "include/rados/buffer.h"
#include "include/rados/librados.hpp"
#include "include/rados/rados_types.hpp"

#include "acconfig.h"

#include "common/config.h"
#include "common/ceph_argparse.h"
#include "global/global_init.h"
#include "common/Cond.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/Formatter.h"
#include "common/obj_bencher.h"

#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <time.h>
#include <sstream>
#include <errno.h>
#include <dirent.h>
#include <stdexcept>
#include <climits>
#include <locale>
#include <memory>

#include "tools/RadosDump.h"

using namespace librados;
unsigned default_op_size = 1 << 22;
uint64_t total_size = 0;
map< string, pair <int, int> > chunk_statistics; // < key, <count, chunk_size> >

void usage()
{
  cout << " usage: [--op_name <op>] [--pool <pool_name> ] <mapfilename>" << std::endl;
  cout << "   --object <object_name>   write osdmap's crush map to <file>" << std::endl;
  cout << "   --chunk-size <file>   replace osdmap's crush map with <file>" << std::endl;
  cout << "   --chunk-algorighm [--pool <poolid>] [--pg_num <pg_num>] map all pgs" << std::endl;
  cout << "   --fingerprint-algorighn [--pool <poolid>] map all pgs" << std::endl;
  exit(1);
}

[[noreturn]] static void usage_exit()
{
  usage();
  exit(1);
}

template <typename I, typename T>
static int rados_sistrtoll(I &i, T *val) {
  std::string err;
  *val = strict_iecstrtoll(i->second.c_str(), &err);
  if (err != "") {
    cerr << "Invalid value for " << i->first << ": " << err << std::endl;
    return -EINVAL;
  } else {
    return 0;
  }
}

int estimate_dedup_ratio(const std::map < std::string, std::string > &opts,
			  std::vector<const char*> &nargs)
{
  Rados rados;
  IoCtx io_ctx;
  std::string object_name;
  std::string chunk_algo;
  string fp_algo;
  string pool_name;
  uint64_t chunk_size = 0;
  unsigned op_size = default_op_size;
  int ret;
  std::map<std::string, std::string>::const_iterator i;

  i = opts.find("pool");
  if (i != opts.end()) {
    pool_name = i->second.c_str();
  }
  i = opts.find("object");
  if (i != opts.end()) {
    object_name = i->second.c_str();
  }
  i = opts.find("chunk-algorithm");
  if (i != opts.end()) {
    chunk_algo = i->second.c_str();
  }
  i = opts.find("fingerprint-algorithm");
  if (i != opts.end()) {
    fp_algo = i->second.c_str();
  }
  i = opts.find("chunk-size");
  if (i != opts.end()) {
    if (rados_sistrtoll(i, &chunk_size)) {
      return -EINVAL;
    }
  }
  i = opts.find("pgid");
  boost::optional<pg_t> pgid(i != opts.end(), pg_t());

  ret = rados.init_with_context(g_ceph_context);
  if (ret < 0) {
     cerr << "couldn't initialize rados: " << cpp_strerror(ret) << std::endl;
     goto out;
  }
  ret = rados.connect();
  if (ret) {
     cerr << "couldn't connect to cluster: " << cpp_strerror(ret) << std::endl;
     ret = -1;
     goto out;
  }
  if (pool_name.empty()) {
    cerr << "--create-pool requested but pool_name was not specified!" << std::endl;
    usage_exit();
  }
  ret = rados.ioctx_create(pool_name.c_str(), io_ctx);
  if (ret < 0) {
    cerr << "error opening pool "
	 << pool_name << ": "
	 << cpp_strerror(ret) << std::endl;
    goto out;
  }

  {
    try {
      librados::NObjectIterator i = pgid ? io_ctx.nobjects_begin(pgid->ps()) : io_ctx.nobjects_begin();
      librados::NObjectIterator i_end = io_ctx.nobjects_end();
      for (; i != i_end; ++i) {
	while (true) {
	  bufferlist outdata;
	  uint64_t offset;
	  ret = io_ctx.read(i->get_oid(), outdata, op_size, 0);
	  if (ret <= 0) {
	    goto out;
	  }

	  if (chunk_algo == "fixed") {
	    if (fp_algo == "sha1") {
		bufferlist chunk;
		uint64_t c_offset = 0;
		while (c_offset < outdata.length()) {
		  if (outdata.length() - c_offset > chunk_size) {
		    chunk.copy_in(c_offset, chunk_size, outdata.c_str());	  
		  } else {
		    chunk.copy_in(c_offset, outdata.length() - c_offset, outdata.c_str());	  
		  }
		  sha1_digest_t sha1_val = chunk.sha1();
		  string fp = sha1_val.to_str();
		  auto p = chunk_statistics.find(fp);
		  if (p != chunk_statistics.end()) {
		    int count = p->second.first;
		    count++;
		    chunk_statistics[fp] = make_pair(count, chunk.length());
		  } else {
		    chunk_statistics[fp] = make_pair(1, chunk.length());
		  }
		  c_offset = c_offset + chunk_size;
		}
	    } else {
	      ceph_assert(0 == "no support fingerprint algorithm"); 
	    }
	  } else {
	    ceph_assert(0 == "no support chunk algorithm"); 
	  }
	  
	  if (outdata.length() < op_size)
	    break;
	  offset += outdata.length();
	}
      }
    }
    catch (const std::runtime_error& e) {
      cerr << e.what() << std::endl;
      ret = -1;
      goto out;
    }
  }

 out:
  return (ret < 0) ? 1 : 0;
}

int chunk_scrub(const std::map < std::string, std::string > &opts,
			  std::vector<const char*> &nargs)
{
  return -1;
}

int main(int argc, const char **argv)
{
  vector<const char*> args;
  argv_to_vec(argc, argv, args);
  if (args.empty()) {
    cerr << argv[0] << ": -h or --help for usage" << std::endl;
    exit(1);
  }
  if (ceph_argparse_need_usage(args)) {
    usage();
    exit(0);
  }

  const char *me = argv[0];

  std::string fn;
  string op_name;

  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  common_init_finish(g_ceph_context);
  std::map < std::string, std::string > opts;
  std::string val;
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "--op", (char*)NULL)) {
      opts["op_name"] = val;
      op_name = val;
      if (op_name != "chunk_scrub" || op_name != "estimate") {
	usage();
      }
    } else if (ceph_argparse_witharg(args, i, &val, "--pool", (char*)NULL)) {
      opts["pool"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--object", (char*)NULL)) {
      opts["object"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--chunk-algorithm", (char*)NULL)) {
      opts["chunk-algorithm"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--chunk-size", (char*)NULL)) {
      opts["chunk-size"] = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--fingerprint-alrorithm", (char*)NULL)) {
      opts["fingerprint-algorithm"] = val;
    } else {
      ++i;
    }
  }
  if (args.empty()) {
    cerr << me << ": you must give an action. Try --help" << std::endl;
    usage();
  }


  if (op_name == "estimate") {
    return estimate_dedup_ratio(opts, args);
  } else if (op_name == "chunk_scrub") {
    return chunk_scrub(opts, args);
  } else {
    ceph_assert(0 == "no support op_name "); 
  }
  
  return 0;
}
