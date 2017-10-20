// logSeismic.cpp
// NOTICE: This must run as root.

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

static const int VERSION = 1;

static const char    *rootPath = "/home/pi";
bool                  run = true;
pthread_t             fileThread;
pthread_mutex_t       fileMutex;
std::deque< Reading > readings;

//////////////////////////////////////////////////////////////////////////////
// Functions

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

// Use for debug.
void adxl345DisplayRegisters(void) {
  uint8_t input[100];

  std::cout << "Reg  Value" << std::endl;
  std::cout << "----+-----" << std::endl;
  for (int i = 0; i < 58; i++) {
    if (i < 1 || i > 28) {
      uint8_t v = adxl345ReadOne(i);
      std::cout << std::hex << "0x" << i
                << " 0x" << (int)v << std::endl;
    }
  }
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
  // Setup BW rate: 100 samples per second.
  adxl345Write(0x2c, 10);
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
void ctrl_c_handler(int s) {
  run = false;
}

double getCurrentTime() {
  using namespace std::chrono;

  high_resolution_clock::time_point th = high_resolution_clock::now();
  high_resolution_clock::duration dth = th.time_since_epoch();
  return duration_cast<duration<double>>(dth).count();
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

// This thread writes readings to the output files.
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

  // Get path to first file.
  time(&t);
  fileStartTime = readingFilePath(t, directoryPath, path);
  fileEndTime = fileStartTime + 86400;

  sleep(15);
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
      readingFile.open(path, std::ios::out | std::ios::app
                       | std::ios::binary);
      std::deque< Reading >::iterator r;
      int n = 0;
      for (r = fileReadings.begin(); r != fileReadings.end(); r++, n++) {
        uint32_t msec =
          (uint32_t)(1000.0 * (r->time - (double)fileStartTime));
        int16_t x = r->values.x;
        int16_t y = r->values.y;
        int16_t z = r->values.z;
        time_t tr = (time_t)(uint32_t)r->time;
        if (tr >= fileEndTime) {
          readingFile.close();
          fileStartTime = readingFilePath(tr, directoryPath, path);
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
  Values  offsets;
  Reading reading;
  
  // Use ctrl-c to quit. Setup handler.
  struct sigaction ctrl_c_action;
  ctrl_c_action.sa_handler = ctrl_c_handler;
  sigemptyset(&ctrl_c_action.sa_mask);
  ctrl_c_action.sa_flags = 0;
  sigaction(SIGINT, &ctrl_c_action, NULL);
  
  std::cout << "testADXL345 version " << VERSION << std::endl;
  std::cout << std::endl
            << " *** Type ctrl-C (^C) to quit *** "
            << std::endl << std::endl;
  std::cout << "Initialize BCM2835 I/O module" << std::endl;
  if (!bcm2835_init()) {
    std::cout << "bcm2835_init() failed" << std::endl;
  }

  std::cout << "Begin SPI" << std::endl;
  if (!bcm2835_spi_begin()) {
    std::cout << "bcm2835_begin() failed" << std::endl;
  }

  std::cout << "Set SPI options" << std::endl;
  adxl345Setup();

  // Check that the ADXL345 is connected.
  uint8_t id = adxl345ReadOne(0);
  if (id != 0xe5) {
    std::cout << "ADXL345 device is not connected, quiting" << std::endl;
    std::cout << "End BCM2835" << std::endl;
    bcm2835_spi_end();
    std::cout << "Close BCM2835" << std::endl;
    bcm2835_close();
    return 1;
  }

  std::cout << "Start ADXL345 accelerometer" << std::endl;
  adxl345Start();

  // Initialize the file mutex.
  fileMutex = PTHREAD_MUTEX_INITIALIZER;

  std::cout << "Start the file thread" << std::endl;
  pthread_create(&fileThread, NULL, fileFunction, (void*)NULL);

  // Use the first 50 readings to set the offset values.
  bool zeroed = false;
  std::cout << "Zeroing..." << std::endl;

  reading.values.x = reading.values.y = reading.values.z = 0;
  offsets.x = offsets.y = offsets.z = 0;
  int count, zeroCount = 0, samples = 0;
  double x, y, z;

  while(run) {
    if ((count = adxl345GetReadings(values)) > 0) {
      for (int i = 0; i < count; i++) {
        // This is a crude 16 stage filter. Replace with FIR.
        reading.values.x += values[i].x;
        reading.values.y += values[i].y;
        reading.values.z += values[i].z;
        if (++samples >= 16) {
          if (zeroed) {
            reading.time = getCurrentTime();
            readings.push_back(reading);
          } else {
            offsets.x += reading.values.x;
            offsets.y += reading.values.y;
            offsets.z += reading.values.z;
            if (++zeroCount >= 50) {
              offsets.x /= 50;
              offsets.y /= 50;
              offsets.z /= 50;
              zeroed = true;
              std::cout << "Logging readings" << std::endl;
            }
          }
          reading.values.x = reading.values.y = reading.values.z = 0;
          samples = 0;
        }
      }
    }
  }

  std::cout << std::endl
            << "Wait for file thread to exit..." << std::endl;
  pthread_join(fileThread, NULL);

  std::cout << "Stop ADXL345" << std::endl;
  adxl345Stop();
  std::cout << "End BCM2835" << std::endl;
  bcm2835_spi_end();
  std::cout << "Close BCM2835" << std::endl;
  bcm2835_close();
  return 0;
}

