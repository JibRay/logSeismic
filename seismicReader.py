#!/usr/bin/env python
#
# Usage:
#  seismicReader.py [-c] <file-name>
#
# where:
#   -c  Use commas to separate fields.
#
# Read and display contents of seismic ibsd file.

import sys
import string
import struct
import time

# Note: when encountering time functions here, remember that within the
# readings file UTC is the local time.

#============================================================================
# Globals
version = 2

#============================================================================
# Functions

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

def displayHelp():
  print 'seismicReader', version
  print 'Usage:'
  print '  seismicReader [-h] [-c] [-f] file-name'
  print 'Where:'
  print '  -h = display this help'
  print '  -c = use commas instead of spaces for column separator'
  print '  -f = output fraction seconds in separate column'

#============================================================================
# Main program

useCommas = False
separateFractionalSecondsColumn = False

if len(sys.argv) < 2:
  displayHelp()
  exit()

if sys.argv[1] == '-h' or sys.argv[1] == '--help':
  displayHelp()
  exit()

for i in range(len(sys.argv)):
  if sys.argv[i] == '-c':
    useCommas = True
  if sys.argv[i] == '-f':
    separateFractionalSecondsColumn = True

fileIndex = len(sys.argv) - 1
inputFile = open(sys.argv[fileIndex], 'rb')
fileTime = getFileTime(sys.argv[fileIndex])
try:
  while 1:
    record = struct.unpack('Ihhh', inputFile.read(10))
    sec = record[0] / 1000
    ts = time.localtime(fileTime + float(sec))
    x = float(record[1]) * 0.24375
    y = float(record[2]) * 0.24375
    z = float(record[3]) * 0.24375
    if separateFractionalSecondsColumn:
      if useCommas:
        fs = '{:4d}-{:02d}-{:02d},{:02d}:{:02d}:{:02d},{:0.3f},' \
          + '{:7.2f},{:7.2f},{:7.2f}'
      else:
        fs = '{:4d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d} {:0.3f} ' \
          + '{:7.2f} {:7.2f} {:7.2f}'
      print fs.format(ts.tm_year, ts.tm_mon, ts.tm_mday, ts.tm_hour, \
        ts.tm_min, ts.tm_sec, float(record[0] % 1000) / 1000.0, x, y, z)
    else:
      if useCommas:
        fs = '{:4d}-{:02d}-{:02d},{:02d}:{:02d}:{:02d}.{:03d},' \
          + '{:7.2f},{:7.2f},{:7.2f}'
      else:
        fs = '{:4d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d} ' \
          + '{:7.2f} {:7.2f} {:7.2f}'
      print fs.format(ts.tm_year, ts.tm_mon, ts.tm_mday, ts.tm_hour, \
        ts.tm_min, ts.tm_sec, record[0] % 1000, x, y, z)
except:
  print
  #type, value, traceback = sys.exc_info()
  #print type, value
inputFile.close()

