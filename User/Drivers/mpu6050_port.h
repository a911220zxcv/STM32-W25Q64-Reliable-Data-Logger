#ifndef MPU6050_PORT_H
#define MPU6050_PORT_H

#include <stdint.h>
#include "mpu6050.h"

/* Platform boundary used by the portable MPU6050 register driver. */
MPU6050_Status_t MPU6050_PortInit(void);
MPU6050_Status_t MPU6050_PortWriteRegister(uint8_t address, uint8_t value);
MPU6050_Status_t MPU6050_PortReadRegisters(uint8_t address,
                                           uint8_t *data,
                                           uint32_t length);

#endif
