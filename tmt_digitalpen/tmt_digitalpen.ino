/**
* TinyML Trainer Example. 
* Version: v005
* 
* Required Libraries:
* - TensorFlow Lite for Microcontrollers (tested with v2.4-beta)
* - Arduino_LSM9DS1
*
* Usage:
* - Make sure the Arduino BLE 33 board is installed (tools->board->Board Manager) and connected
* - Make sure you have the required libraries installed
* - Build and upload the sketch
* - Keep the Arduino connected via USB and open the Serial Monitor (Tools->Serial Monitor)
*
**/

//==============================================================================
// Includes
//==============================================================================

#include <Arduino_LSM9DS1.h>
#include <TensorFlowLite.h>
#include <tensorflow/lite/micro/all_ops_resolver.h>
#include <tensorflow/lite/micro/micro_error_reporter.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/schema/schema_generated.h>
#include <tensorflow/lite/version.h>


//==============================================================================
// Your custom data / settings
// - Editing these is not recommended
//==============================================================================

// This is the model you trained in Tiny Motion Trainer, converted to 
// a C style byte array.
#include "model.h"

// Values from Tiny Motion Trainer
#define MOTION_THRESHOLD 0.1
#define CAPTURE_DELAY 1000 // This is now in milliseconds
#define NUM_SAMPLES 100

// Array to map gesture index to a name
const char *GESTURES[] = {
    "H", "A", "L", "O", "B", "C", "D", "E", "F", "G", "I", "J", "K", "M", "N", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
};


//==============================================================================
// Capture variables
//==============================================================================

#define NUM_GESTURES (sizeof(GESTURES) / sizeof(GESTURES[0]))

bool isCapturing = false;

// Num samples read from the IMU sensors
// "Full" by default to start in idle
int numSamplesRead = 0;

bool skip_next_sample = false;


//==============================================================================
// TensorFlow variables
//==============================================================================

// Global variables used for TensorFlow Lite (Micro)
tflite::MicroErrorReporter tflErrorReporter;

// Auto resolve all the TensorFlow Lite for MicroInterpreters ops, for reduced memory-footprint change this to only 
// include the op's you need.
tflite::AllOpsResolver tflOpsResolver;

// Setup model
const tflite::Model* tflModel = nullptr;
tflite::MicroInterpreter* tflInterpreter = nullptr;
TfLiteTensor* tflInputTensor = nullptr;
TfLiteTensor* tflOutputTensor = nullptr;

// Create a static memory buffer for TensorFlow Lite for MicroInterpreters, the size may need to
// be adjusted based on the model you are using
constexpr int tensorArenaSize = 8 * 1024;
byte tensorArena[tensorArenaSize];


//==============================================================================
// Setup / Loop
//==============================================================================

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
        
  Serial.begin(9600);

  // Wait for serial monitor to connect
  while (!Serial);

  // Initialize IMU sensors
  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU!");
    while (1);
  }

  // Print out the samples rates of the IMUs
  Serial.print("Accelerometer sample rate: ");
  Serial.print(IMU.accelerationSampleRate());
  Serial.println(" Hz");
  Serial.print("Gyroscope sample rate: ");
  Serial.print(IMU.gyroscopeSampleRate());
  Serial.println(" Hz");

  Serial.println();

  // Get the TFL representation of the model byte array
  tflModel = tflite::GetModel(model);
  if (tflModel->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema mismatch!");
    while (1);
  }

  // Create an interpreter to run the model
  tflInterpreter = new tflite::MicroInterpreter(tflModel, tflOpsResolver, tensorArena, tensorArenaSize, &tflErrorReporter);

  // Allocate memory for the model's input and output tensors
  tflInterpreter->AllocateTensors();

  // Get pointers for the model's input and output tensors
  tflInputTensor = tflInterpreter->input(0);
  tflOutputTensor = tflInterpreter->output(0);
}

void loop() {
  // Variables to hold IMU data
  float aX, aY, aZ, gX, gY, gZ;

  // Wait for motion above the threshold setting
  while (!isCapturing) {
    if (IMU.accelerationAvailable() && IMU.gyroscopeAvailable()) {
     
      IMU.readAcceleration(aX, aY, aZ);
      IMU.readGyroscope(gX, gY, gZ);

      // Sum absolute values
      float average = fabs(aX / 4.0) + fabs(aY / 4.0) + fabs(aZ / 4.0) + fabs(gX / 2000.0) + fabs(gY / 2000.0) + fabs(gZ / 2000.0);
      average /= 6.;

      // Above the threshold?
      if (average >= MOTION_THRESHOLD) {
        isCapturing = true;
        skip_next_sample = false;
        numSamplesRead = 0;
        break;
      }
    }
  }

  while (isCapturing) {

    // Check if both acceleration and gyroscope data is available
    if (IMU.accelerationAvailable() && IMU.gyroscopeAvailable()) {

      // read the acceleration and gyroscope data
      IMU.readAcceleration(aX, aY, aZ);
      IMU.readGyroscope(gX, gY, gZ);

      if (skip_next_sample) {
        skip_next_sample = false;
      } else {
        skip_next_sample = true;

        // Normalize the IMU data between -1 to 1 and store in the model's
        // input tensor. Accelerometer data ranges between -4 and 4,
        // gyroscope data ranges between -2000 and 2000
        tflInputTensor->data.f[numSamplesRead * 6 + 0] = aX / 4.0;
        tflInputTensor->data.f[numSamplesRead * 6 + 1] = aY / 4.0;
        tflInputTensor->data.f[numSamplesRead * 6 + 2] = aZ / 4.0;
        tflInputTensor->data.f[numSamplesRead * 6 + 3] = gX / 2000.0;
        tflInputTensor->data.f[numSamplesRead * 6 + 4] = gY / 2000.0;
        tflInputTensor->data.f[numSamplesRead * 6 + 5] = gZ / 2000.0;
  
        numSamplesRead++;
  
        // Do we have the samples we need?
        if (numSamplesRead == NUM_SAMPLES) {
          
          // Stop capturing
          isCapturing = false;
          
          // Run inference
          TfLiteStatus invokeStatus = tflInterpreter->Invoke();
          if (invokeStatus != kTfLiteOk) {
            Serial.println("Error: Invoke failed!");
            while (1);
            return;
          }
  
          // Loop through the output tensor values from the model
          int maxIndex = 0;
          int secIndex = 0;
          float maxValue = 0;
          float secValue = 0;
          for (int i = 0; i < NUM_GESTURES; i++) {
            float _value = tflOutputTensor->data.f[i];
            if(_value > maxValue){
              secValue = maxValue;
              maxValue = _value;
              secIndex = maxIndex;
              maxIndex = i;
            }
          }
          
          Serial.print(GESTURES[maxIndex]);
          Serial.print(' ');
          Serial.print(int(maxValue * 100.));
          Serial.print('%');

          Serial.print('\t');
          
          Serial.print(GESTURES[secIndex]);
          Serial.print(' ');
          Serial.print(int(secValue * 100.));
          Serial.print('%');
          
          Serial.println();
  
          // Add delay to not double trigger
          delay(CAPTURE_DELAY);
        }
      }
    }
  }
}
