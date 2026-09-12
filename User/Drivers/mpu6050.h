#ifndef MPU6050_H
#define MPU6050_H

#include <stdint.h>

#define MPU6050_EXPECTED_WHO_AM_I 0x68U

typedef enum
{
    MPU6050_OK = 0,
    MPU6050_ERROR,
    MPU6050_TIMEOUT,
    MPU6050_INVALID_PARAM,
    MPU6050_NOT_INITIALIZED,
    MPU6050_NOT_CONFIGURED,
    MPU6050_UNSUPPORTED_DEVICE,
    MPU6050_VERIFY_ERROR
} MPU6050_Status_t;

typedef struct
{
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} MPU6050_Sample_t;

/* Initialize I2C, check WHO_AM_I and apply the documented sample setup. */
MPU6050_Status_t MPU6050_Init(void);
/* Read WHO_AM_I. Output is modified only on success. */
MPU6050_Status_t MPU6050_ReadDeviceId(uint8_t *device_id);
/* Read accel, temperature and gyro registers in one 14-byte transaction;
 * temperature is intentionally discarded by the Phase 3 record format. */
MPU6050_Status_t MPU6050_ReadSample(MPU6050_Sample_t *sample);

#endif
