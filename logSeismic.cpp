// logSeismic.cpp
// NOTICE: This must run as root.

#include <sys/types.h>
#include <stdlib.h>
#include <syslog.h>
#include <iostream>
#include <fstream>
#include <string.h>
#include <iomanip>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <deque>
#include <pthread.h>
#include <chrono>
#include <bcm2835.h>
#include <unistd.h>

// Note about ADXL345 SPI:
// This program uses a separate GPIO pin to control the SPI CE signal. The
// ADXL345 requires that CE remain asserted while reading all 6 data
// registers. See the ADXL345 datasheet, adxl345Setup() and
// adxl345GetReadings().

//////////////////////////////////////////////////////////////////////////////
// Data types and globals

struct Values {
  int x;
  int y;
  int z;
};

struct Reading {
  double time;
  Values values;
};

static const int VERSION = 4;

static const char    *rootPath = "/home/pi";
char                  catalogPath[200];
char                  newFilePath[200];
bool                  initialized = false, run = true, newDay = false;
pthread_t             fileThread, catalogThread;
pthread_mutex_t       fileMutex;
std::deque< Reading > readings;
Values                offsets;

//////////////////////////////////////////////////////////////////////////////
// Functions

void writeLog(const char *message) {
  char line[200];
  std::ofstream logFile;
  struct stat info;

  if (stat("/var/log/logSeismic", &info) != 0 || !S_ISDIR(info.st_mode))
    mkdir("/var/log/logSeismic",
      S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

  struct tm *ts;
  time_t t;
  time(&t);
  ts = localtime(&t);
  sprintf(line, "%4d-%02d-%02d %02d:%02d:%02d %s\n",
    ts->tm_year, ts->tm_mon, ts->tm_mday, ts->tm_hour, ts->tm_min, ts->tm_sec,
    message);
  logFile.open("/var/log/logSeismic/logSeismic.log",
    std::ios::out | std::ios::app);
  logFile << line;
  logFile.close();
}

// This delay function is needed to meet the CE off time requirements.
int delay(int t) {
  // Structure this so the compiler won't optimize it away.
  int d = 0;
  for (int i = 0; i < t; i++)
    d += 1;
  return d;
}

// Call this function repeatedly to do multi-register reads. The calling
// function must control the CE bit. Return the register value.
uint8_t adxl345Read(int reg) {
  uint8_t output[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t input[8];

  output[0] = (uint8_t)(reg | 0x80);
  bcm2835_spi_transfernb((char*)output, (char*)input, 2);
  return input[1];
}

// Read a single SPI register. CE is handled here. Return the register
// value.
uint8_t adxl345ReadOne(int reg) {
  bcm2835_gpio_clr(RPI_BPLUS_GPIO_J8_24);
  uint8_t v = adxl345Read(reg);
  bcm2835_gpio_set(RPI_BPLUS_GPIO_J8_24);
  delay(20); // About 280 nanoseconds.
  return v;
}

// Write a single SPI register. CE is handled here.
void adxl345Write(int reg, uint8_t value) {
  uint8_t output[4];
  uint8_t input[4];

  output[0] = (uint8_t)(reg & 0x7f);
  output[1] = value;
  bcm2835_gpio_clr(RPI_BPLUS_GPIO_J8_24);
  bcm2835_spi_writenb((char*)output, 2);
  bcm2835_gpio_set(RPI_BPLUS_GPIO_J8_24);
  delay(20); // About 280 nanoseconds.
}

// Read a contiguous group of SPI registers. Values are returnd in
// values. Also return a pointer to values.
uint8_t *adxl345ReadRegisters(int reg, uint8_t *values, int count) {
  bcm2835_gpio_clr(RPI_BPLUS_GPIO_J8_24);
  for (int i = 0; i < count; i++) {
    values[i] = adxl345Read(reg + i);
  }
  bcm2835_gpio_set(RPI_BPLUS_GPIO_J8_24);
  delay(300); // About 5 microseconds.
  return values;
}

// Setup the accelerometer.
void adxl345Setup(void) {
  bcm2835_gpio_fsel(RPI_BPLUS_GPIO_J8_24, BCM2835_GPIO_FSEL_OUTP);
  bcm2835_spi_setClockDivider(BCM2835_SPI_CLOCK_DIVIDER_64);
  bcm2835_spi_setDataMode(BCM2835_SPI_MODE3);
  // Initialize with no CE control. CE must be controlled by code to do
  // multi-register reads.
  bcm2835_spi_chipSelect(BCM2835_SPI_CS_NONE);
  //bcm2835_spi_setChipSelectPolarity(0, 0);
  bcm2835_gpio_set(RPI_BPLUS_GPIO_J8_24);
}

void adxl345Start() {
  // Clear measure bit.
  adxl345Write(0x2d, (uint8_t)0);
  // Setup data format: no self test, 4 wire SPI, no interrupt invert,
  // full resolution, right justify, range = +/- 2g.
  adxl345Write(0x31, 0b00001000);
  // Setup the FIFO: stream mode, no trigger, 16 samples.
  adxl345Write(0x38, 0b10001000);
  // Setup BW rate: 1600 samples per second.
  adxl345Write(0x2c, 0x0f);
  // Setup power control: no inactivity, no auto-sleep, measure, no sleep,
  // wakeup bit = 00. The measure bit starts the sampling.
  adxl345Write(0x2d, 0b00001000);
}

void adxl345Stop() {
  // Clear the measure bit.
  adxl345Write(0x2d, 0);
}

// Return the X, Y, Z readings in values. These are 16 bit signed values.
// They are retrieved from the ADXL345 FIFO. Return the number of readings
// obtained from the FIFO which may be zero.
int adxl345GetReadings(Values *values) {
  uint8_t input[100];

  // Get the number of readings in the FIFO.
  int count = adxl345ReadOne(0x39) & 0x3f;
  if (count > 0) {
    for (int i = 0; i < count; i++) {
      // Each axis reading is two bytes for a total of 6 registers.
      adxl345ReadRegisters(0x32, input, 6);
      values[i].x = (int16_t)(input[0] + (input[1] << 8));
      values[i].y = (int16_t)(input[2] + (input[3] << 8));
      values[i].z = (int16_t)(input[4] + (input[5] << 8));
    }
  }
  return count;
}

// Ctrl-C signal call-back.
/*
void ctrl_c_handler(int s) {
  run = false;
}
*/
void signalHandler(int n) {
  switch (n) {
    case SIGTERM:
      run = false;
      break;
  }
}

double getTimeZoneOffset() {
  struct tm ts;
  ts.tm_year = 117;
  ts.tm_mon = 1;
  ts.tm_mday = 1;

  time_t utc = timegm(&ts);
  time_t local = timelocal(&ts);

  return (double)(local - utc);
}

// Return high-resolution UTC time as a double.
double getCurrentTime(double timeZoneOffset) {
  using namespace std::chrono;

  high_resolution_clock::time_point th = high_resolution_clock::now();
  high_resolution_clock::duration dth = th.time_since_epoch();
  return timeZoneOffset + duration_cast<duration<double>>(dth).count();
}

// Create a file path from basePath and the time value t. Return the
// new path in path. The file name is YYYY-MM-DD.ibsd. YYYY-MM-DD is the
// day that include t. Return the time_t value that is the start of the
// day specified in the file name.
time_t readingFilePath(time_t t, char *basePath, char *path) {
  struct tm *pt;

  pt = gmtime(&t);
  sprintf(path, "%s/%04d-%02d-%2d.ibsd",
          basePath, 1900 + pt->tm_year, 1 + pt->tm_mon, pt->tm_mday);
  pt->tm_hour = pt->tm_min = pt->tm_sec = 0;
  return mktime(pt);
}

void *catalogFunction(void *param) {

  while (run) {
    sleep(60);
    if (newDay) {
      newDay = false;
    }
  }
  return NULL;
}

// This thread writes readings to the output files. It wakes up every 5
// seconds and checks the global queue for readings. All readings found
// are transfered to a local queue and then written to the output file.
// Each file record is 10 byes. First four are unsigned number of
// milliseconds since the start of the UTC day. This is followed by three
// 16 bit signed values, X, Y and Z acceleration. To convert these values
// to milli-g, multiply by 0.24375.
//
// The file name is year-month-day.ibsd. When UTC time in the next record
// rolls over from one day to the next the current file is closed and a
// new one is created.
void *fileFunction(void *param) {
  std::deque< Reading > fileReadings;
  struct stat info;
  char directoryPath[200], path[200];
  int i, count;
  time_t t, fileStartTime, fileEndTime;
  struct tm *pt;
  std::ofstream readingFile;
  uint8_t recordBuffer[11];

  // Create any missing directories.
  strcpy(directoryPath, rootPath);
  strcat(directoryPath, "/seismometer");
  if (stat(directoryPath, &info) != 0 || !S_ISDIR(info.st_mode))
    mkdir(directoryPath, S_IRWXG | S_IRWXU | S_IRWXO);
  strcat(directoryPath, "/readings");
  if (stat(directoryPath, &info) != 0 || !S_ISDIR(info.st_mode))
    mkdir(directoryPath, S_IRWXG | S_IRWXU | S_IRWXO);

  strcpy(catalogPath, directoryPath);

  // Get path to first file.
  time(&t);
  fileStartTime = readingFilePath(t, directoryPath, path);
  fileEndTime = fileStartTime + 86400;

  // Wait for the main thread to initialize.
  while (!initialized)
    sleep(1);

  while (run) {
    pthread_mutex_lock(&fileMutex);
    // Transfer all readings from global queue to local queue.
    count = (int)readings.size();
    if (count > 0) {
      for (i = 0; i < count; i++) {
        fileReadings.push_back(readings.front());
        readings.pop_front();
      }
    }
    pthread_mutex_unlock(&fileMutex);

    if (fileReadings.size() > 0) {
      // Local queue contains readings.
      readingFile.open(path, std::ios::out | std::ios::app
                       | std::ios::binary);
      std::deque< Reading >::iterator r;
      for (r = fileReadings.begin(); r != fileReadings.end(); r++) {
        uint32_t msec =
          (uint32_t)(1000.0 * (r->time - (double)fileStartTime));
        int16_t x = r->values.x - offsets.x;
        int16_t y = r->values.y - offsets.y;
        int16_t z = r->values.z - offsets.z;
        time_t rTime = (time_t)(uint32_t)r->time;
        if (rTime >= fileEndTime) {
          // We've reached the end of the day. Let the catalog thread
          // know about the newly completed day file.
          readingFile.close();
          strcpy(newFilePath, path);
          newDay = true;

          fileStartTime = readingFilePath(rTime, directoryPath, path);
          fileEndTime = fileStartTime + 86400;
          readingFile.open(path, std::ios::out | std::ios::app
                           | std::ios::binary);
        }
        recordBuffer[0] = msec & 0xff;
        recordBuffer[1] = (msec >> 8) & 0xff;
        recordBuffer[2] = (msec >> 16) & 0xff;
        recordBuffer[3] = (msec >> 24) & 0xff;
        recordBuffer[4] = x & 0xff;
        recordBuffer[5] = (x >> 8) & 0xff;
        recordBuffer[6] = y & 0xff;
        recordBuffer[7] = (y >> 8) & 0xff;
        recordBuffer[8] = z & 0xff;
        recordBuffer[9] = (z >> 8) &0xff;
        readingFile.write((char*)recordBuffer, 10);
      }
      readingFile.close();
      fileReadings.clear();
    }
    sleep(5);
  }
  return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// Main program.

int main() {
  uint8_t output[100];
  uint8_t input[100];
  Values  values[33];
  Reading reading;
  char    message[200];

  setlogmask(LOG_UPTO(LOG_NOTICE));
  openlog("logSeismic", LOG_CONS | LOG_NDELAY | LOG_PERROR | LOG_PID,
          LOG_USER);

  pid_t pid, sid;

  // Fork the parent process.
  pid = fork();
  if (pid < 0) {
    syslog(LOG_ERR, "process fork failed");
    exit(1);
  }
  if (pid > 0)
    exit(0);  // Close parent process, continue only with daemon.

  // Change file mask.
  umask(0);

  // Create a new signature ID for the child process.
  sid = setsid();
  if (sid < 0) {
    syslog(LOG_ERR, "setsid() failed");
    exit(1);
  }

  // Change directory to root.
  if (chdir("/") < 0) {
    syslog(LOG_ERR, "chdir to root directory failed");
    exit(1);
  }

  // Close std file descriptors.
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  sprintf(message, "Start logSeismic version %d", VERSION);
  syslog(LOG_NOTICE, message);
  double  timeZoneOffset = getTimeZoneOffset();
  
  // Setup signal handlers.
  signal(SIGTERM, signalHandler);
  /*
  struct sigaction ctrl_c_action;
  ctrl_c_action.sa_handler = ctrl_c_handler;
  sigemptyset(&ctrl_c_action.sa_mask);
  ctrl_c_action.sa_flags = 0;
  sigaction(SIGINT, &ctrl_c_action, NULL);
  */
  
  sprintf(message, "logSeismic version %d", VERSION);
  writeLog(message);
  writeLog("Initialize BCM2835 I/O module");
  if (!bcm2835_init()) {
    writeLog("bcm2835_init() failed");
  }

  writeLog("Begin SPI");
  if (!bcm2835_spi_begin()) {
    writeLog("bcm2835_begin() failed");
  }

  writeLog("Set SPI options");
  adxl345Setup();

  // Check that the ADXL345 is connected.
  uint8_t id = adxl345ReadOne(0);
  if (id != 0xe5) {
    writeLog("ADXL345 device is not connected, quiting");
    writeLog("End BCM2835");
    bcm2835_spi_end();
    writeLog("Close BCM2835");
    bcm2835_close();
    exit(1);
  }

  writeLog("Start ADXL345 accelerometer");
  adxl345Start();

  // Initialize the file mutex.
  fileMutex = PTHREAD_MUTEX_INITIALIZER;

  writeLog("Start the file thread");
  pthread_create(&fileThread, NULL, fileFunction, (void*)NULL);
  writeLog("Start the catalog thread");
  pthread_create(&catalogThread, NULL, catalogFunction, (void*)NULL);

  // Use the first 50 readings to set the offset values.
  bool zeroed = false;
  writeLog("Zeroing...");

  reading.values.x = reading.values.y = reading.values.z = 0;
  offsets.x = offsets.y = offsets.z = 0;
  int count, zeroCount = 0, samples = 0;
  double x, y, z;

  while(run) {
    if ((count = adxl345GetReadings(values)) > 0) {
      for (int i = 0; i < count; i++) {
        // This is a 16 stage filter (FIR with constant coefficients).
        reading.values.x += values[i].x;
        reading.values.y += values[i].y;
        reading.values.z += values[i].z;
        if (++samples >= 16) {
          if (zeroed) {
            reading.time = getCurrentTime(timeZoneOffset);
            readings.push_back(reading);
          } else {
            offsets.x += reading.values.x;
            offsets.y += reading.values.y;
            offsets.z += reading.values.z;
            if (++zeroCount >= 50) {
              offsets.x /= zeroCount;
              offsets.y /= zeroCount;
              offsets.z /= zeroCount;
              // Zeroing has completed.
              zeroed = true;
              initialized = true;
              writeLog("Logging readings");
            }
          }
          reading.values.x = reading.values.y = reading.values.z = 0;
          samples = 0;
        }
      }
    }
  }

  writeLog("Wait for file thread to exit...");
  pthread_join(fileThread, NULL);
  writeLog("Wait for catalog thread to exit...");
  pthread_join(catalogThread, NULL);

  writeLog("Stop ADXL345");
  adxl345Stop();
  writeLog("End BCM2835");
  bcm2835_spi_end();
  writeLog("Close BCM2835");
  bcm2835_close();
  syslog(LOG_NOTICE, "logSiesmic stopped");
  closelog();
  exit(0);
}

