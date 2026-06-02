#!/usr/bin/env python
# encoding: utf-8
###########################################
# recover data block from ext_scan result
# by curu, 2020-03-03
# no rights reserved, share if it helps.
###########################################
import os
import sys
import re
import argparse
import errno
import traceback

block_size=4096

def parse_extent_info(extent_info_file):
	fh = open(extent_info_file)
	extents = fh.read().strip().split("\n\n")
	fh.close()
	extent_dict = {}
	for extent in extents:
		ext_lines = extent.split("\n")
		ext_addr = long(re.search(r'at block:(\d+)', ext_lines[0]).group(1))
		total_len = 0
		block_start_addr = 0
		extent_dict[ext_addr] = { "total_len":0, "start_addr":0, "blocks": []}
		for line in ext_lines:
			match = re.search(r'ee_len:(\d+) .*LEAF_BLOCK_ADDR:(\d+)', line)
			if not match:
				continue
			len = long(match.group(1))
			total_len += long(len)
			if not block_start_addr:
				block_start_addr = match.group(2)
			extent_dict[ext_addr]["blocks"].append({"len": len, "start": long(match.group(2))})
		extent_dict[ext_addr]["total_len"] = total_len
		extent_dict[ext_addr]["start_addr"] = block_start_addr
	return extent_dict

def print_stat(extent_info):
	for ext_addr, ext in sorted(extent_info.items(), key=lambda  i: i[1]["total_len"], reverse=True):
		s = "extent:%-14s start_addr:%-14s len:%-14d (%.2fMB)\n" % (
			ext_addr, ext["start_addr"], ext["total_len"], ext["total_len"]*4/1024.0)
		try:
			sys.stdout.write(s)
		except IOError as e:
			break

def sendfile(ofh, ifh,  offset, len):
	if hasattr(os, 'sendfile'):
		os.sendfile(ofh, ifh, block["start"]*block_size, block["len"]*block_size)
	else:
		os.lseek(ifh, offset, os.SEEK_SET);
		os.write(ofh, os.read(ifh, len))
 
def dump_extent(extent, input_file, output_file):
	global block_size
	ifh = os.open(input_file, os.O_RDONLY)
	ofh = os.open(output_file, os.O_WRONLY|os.O_CREAT|os.O_EXCL)
	for block in extent["blocks"]:
		sendfile(ofh, ifh, block["start"]*block_size, block["len"]*block_size)
	os.close(ifh)
	os.close(ofh)
	
	
if __name__ == "__main__":
	if len(sys.argv) < 3:
		print >>sys.stderr, "Usage:"
		print >>sys.stderr, "    %s stat /path/to/extent_file" % sys.argv[0]
		print >>sys.stderr, "    %s dump /path/to/extent_file extent_id device/image output_file"	 % sys.argv[0]
		sys.exit(1)
	action = sys.argv[1]
	extent_file = sys.argv[2]
	if not os.path.exists(extent_file):
		print >>sys.stderr, "ERROR: extent file %s not exists" % extent_file
		sys.exit(1)
	
	try:
		extent_info = parse_extent_info(extent_file)
	except Exception, e:
		print >>sys.stderr, "ERROR: parse extent file error: \n%s" % traceback.format_exc()
		sys.exit(1)
	if action == "stat":
		print_stat(extent_info)
	if action == "dump":
		if len(sys.argv) < 6:
			print >>sys.stderr, "Usage: %s dump /path/to/extent_file extent_id device/image output_file"	% sys.argv[0]
			sys.exit(1)
		extent_addr, input_file, output_file = sys.argv[3:]
		if os.path.exists(output_file):
			print >>sys.stderr, "ERROR: output file '%s' exists!" % output_file
			sys.exit(1)
		extent = extent_info.get(long(extent_addr))
		if extent:
			print "dump extent %s from '%s' to '%s'" % (extent_addr, input_file, output_file)
			dump_extent(extent, input_file, output_file)
		else:
			print "ERROR: extent %s not found, try stat first" % extent_addr
			sys.exit(1)
		
