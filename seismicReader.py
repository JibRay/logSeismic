#!/usr/bin/env python
#
# Usage:
#  seismicReader.py <file-name>
#
# Read and display contents of seismic ibsd file.

import sys
import string
import struct
import time

# Note: when encountering time functions here, remember that within the
# readings file UTC is the local time.

def getFileTime(path):
  i = string.rfind(path, '/')
  if i >= 0:
    fileName = path[i+1:]
  else:
    filename = path
  i = string.find(fileName, '.')
  if i >= 0:
    fileName = fileName[:i]
  ts = time.strptime(fileName, '%Y-%m-%d')
  return time.mktime(ts)

inputFile = open(sys.argv[1], 'rb')
fileTime = getFileTime(sys.argv[1])
try:
  while 1:
    record = struct.unpack('Ihhh', inputFile.read(10))
    sec = record[0] / 1000
    ts = time.localtime(fileTime + float(sec))
    x = float(record[1]) * 0.24375
    y = float(record[2]) * 0.24375
    z = float(record[3]) * 0.24375
    fs = '{:4d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d} ' \
      + '{:7.2f} {:7.2f} {:7.2f}'
    print fs.format(ts.tm_year, ts.tm_mon, ts.tm_mday, ts.tm_hour, \
      ts.tm_min, ts.tm_sec, record[0] % 1000, x, y, z)
except:
  print
inputFile.close()

