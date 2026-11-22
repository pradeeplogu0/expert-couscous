#pragma once
#include <Arduino.h>

// Initialize LSM6DS3TR-C and start the 100 Hz pedometer FreeRTOS task.
void    imu_init();

// True if the LSM6DS3 IMU answered (not fitted on XIAO ESP32-S3 + Round Display).
bool    imu_present();

// Shared state — safe to read from any task (atomic 32-bit on ESP32).
uint32_t imu_get_steps();
void     imu_reset_steps();
bool     imu_is_walking();
int      imu_get_cadence_spm();
int      imu_get_ax_mg();
int      imu_get_ay_mg();
int      imu_get_az_mg();
int      imu_get_mag_mg();

// Sensitivity 1 (less sensitive) … 10 (most sensitive). Default 5.
void    imu_set_sensitivity(uint8_t s);
uint8_t imu_get_sensitivity();
