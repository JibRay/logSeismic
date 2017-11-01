#!/usr/bin/env python
# Read and display contents of seismic ibsd file.
#
# Usage:
#  seismicReader.py [-c] <file-name>
#
# See displayHelp for description of arguments.

import sys
import string
import struct
import time

# Note: when encountering time functions here, remember that within the
# readings file UTC is the local time.

#============================================================================
# Globals
version = 5

#============================================================================
# Functions

def getFileTime(path):
  i = string.rfind(path, '/')
  fileName = ''
  if i >= 0:
    fileName = path[i+1:]
  else:
    fileName = path
  i = string.find(fileName, '.')
  if i >= 0:
    fileName = fileName[:i]
  ts = time.strptime(fileName, '%Y-%m-%d')
  return time.mktime(ts)

def displayHelp():
  print 'seismicReader version', version
  print 'Usage:'
  print '  seismicReader [-h] [-c] [-f] file-name'
  print 'Where:'
  print '  -h = display this help'
  print '  -s = display only statistics as output'
  print '  -c = use commas instead of spaces for column separator'
  print '  -f = output fractional seconds in separate column'
  print 'Output is time, X, Y, Z where X,Y,Z are in micro-g.'

#============================================================================
# Main program

useCommas = False
separateFractionalSecondsColumn = False
onlyStatistics = False

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
  if sys.argv[i] == '-s':
    onlyStatistics = True

fileIndex = len(sys.argv) - 1
inputFile = open(sys.argv[fileIndex], 'rb')
fileTime = getFileTime(sys.argv[fileIndex])
maxAcceleration = [0.0, 0.0, 0.0]
netAcceleration = [0.0, 0.0, 0.0]
try:
  while 1:
    record = struct.unpack('Ihhh', inputFile.read(10))
    sec = record[0] / 1000
    ts = time.localtime(fileTime + float(sec))
    x = float(record[1]) * 0.24375
    y = float(record[2]) * 0.24375
    z = float(record[3]) * 0.24375
    netAcceleration[0] += x
    netAcceleration[1] += y
    netAcceleration[2] += z
    if abs(x) > maxAcceleration[0]:
      maxAcceleration[0] = abs(x)
    if abs(y) > maxAcceleration[1]:
      maxAcceleration[1] = abs(y)
    if abs(z) > maxAcceleration[2]:
      maxAcceleration[2] = abs(z)
    if not onlyStatistics:
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
  # If there is no output, uncomment the following two lines to find out
  # what is wrong.
  type, value, traceback = sys.exc_info()
  print type, value
inputFile.close()

print 'Net accelerations (milli-G):'
print '    X =', netAcceleration[0]
print '    Y =', netAcceleration[1]
print '    Z =', netAcceleration[2]
print 'Max accelerations (milli-G):'
print '    X =', maxAcceleration[0]
print '    Y =', maxAcceleration[1]
print '    Z =', maxAcceleration[2]

