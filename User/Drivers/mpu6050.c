#include "mpu6050.h"
#include "mpu6050_port.h"
#include <stddef.h>

#define MPU6050_REG_SMPLRT_DIV    0x19U
#define MPU6050_REG_CONFIG        0x1AU
#define MPU6050_REG_GYRO_CONFIG   0x1BU
#define MPU6050_REG_ACCEL_CONFIG  0x1CU
#define MPU6050_REG_ACCEL_XOUT_H  0x3BU
#define MPU6050_REG_PWR_MGMT_1    0x6BU
#define MPU6050_REG_PWR_MGMT_2    0x6CU
#define MPU6050_REG_WHO_AM_I      0x75U

#define MPU6050_SAMPLE_BYTES      14UL
#define MPU6050_PWR_MGMT_1_VALUE  0x01U
#define MPU6050_PWR_MGMT_2_VALUE  0x00U
#define MPU6050_SMPLRT_DIV_VALUE  0x09U
#define MPU6050_CONFIG_VALUE      0x06U
#define MPU6050_GYRO_CONFIG_VALUE 0x18U
#define MPU6050_ACCEL_CONFIG_VALUE 0x18U

static uint8_t initialized;

static int16_t decode_i16_be(const uint8_t *data)
{
    uint16_t value = (uint16_t)(((uint16_t)data[0] << 8U) |
                                (uint16_t)data[1]);
    if (value <= 0x7FFFU) { return (int16_t)value; }
    return (int16_t)((int32_t)value - 65536L);
}

static MPU6050_Status_t configure_register(uint8_t address, uint8_t value)
{
    uint8_t actual;
    MPU6050_Status_t status = MPU6050_PortWriteRegister(address, value);
    if (status != MPU6050_OK) { return status; }
    status = MPU6050_PortReadRegisters(address, &actual, 1U);
    if (status != MPU6050_OK) { return status; }
    return actual == value ? MPU6050_OK : MPU6050_VERIFY_ERROR;
}

MPU6050_Status_t MPU6050_Init(void)
{
    uint8_t device_id;
    MPU6050_Status_t status;
    initialized = 0U;
    status = MPU6050_PortInit();
    if (status != MPU6050_OK) { return status; }
    status = MPU6050_PortReadRegisters(MPU6050_REG_WHO_AM_I, &device_id, 1U);
    if (status != MPU6050_OK) { return status; }
    if (device_id != MPU6050_EXPECTED_WHO_AM_I)
    {
        return MPU6050_UNSUPPORTED_DEVICE;
    }
    status = configure_register(MPU6050_REG_PWR_MGMT_1,
                                MPU6050_PWR_MGMT_1_VALUE);
    if (status != MPU6050_OK) { return status; }
    status = configure_register(MPU6050_REG_PWR_MGMT_2,
                                MPU6050_PWR_MGMT_2_VALUE);
    if (status != MPU6050_OK) { return status; }
    status = configure_register(MPU6050_REG_SMPLRT_DIV,
                                MPU6050_SMPLRT_DIV_VALUE);
    if (status != MPU6050_OK) { return status; }
    status = configure_register(MPU6050_REG_CONFIG, MPU6050_CONFIG_VALUE);
    if (status != MPU6050_OK) { return status; }
    status = configure_register(MPU6050_REG_GYRO_CONFIG,
                                MPU6050_GYRO_CONFIG_VALUE);
    if (status != MPU6050_OK) { return status; }
    status = configure_register(MPU6050_REG_ACCEL_CONFIG,
                                MPU6050_ACCEL_CONFIG_VALUE);
    if (status != MPU6050_OK) { return status; }
    initialized = 1U;
    return MPU6050_OK;
}

MPU6050_Status_t MPU6050_ReadDeviceId(uint8_t *device_id)
{
    if (device_id == NULL) { return MPU6050_INVALID_PARAM; }
    if (!initialized) { return MPU6050_NOT_INITIALIZED; }
    return MPU6050_PortReadRegisters(MPU6050_REG_WHO_AM_I, device_id, 1U);
}

MPU6050_Status_t MPU6050_ReadSample(MPU6050_Sample_t *sample)
{
    uint8_t data[MPU6050_SAMPLE_BYTES];
    MPU6050_Sample_t result;
    MPU6050_Status_t status;
    if (sample == NULL) { return MPU6050_INVALID_PARAM; }
    if (!initialized) { return MPU6050_NOT_INITIALIZED; }
    status = MPU6050_PortReadRegisters(MPU6050_REG_ACCEL_XOUT_H,
                                       data, sizeof(data));
    if (status != MPU6050_OK) { return status; }
    result.accel_x = decode_i16_be(data + 0U);
    result.accel_y = decode_i16_be(data + 2U);
    result.accel_z = decode_i16_be(data + 4U);
    result.gyro_x = decode_i16_be(data + 8U);
    result.gyro_y = decode_i16_be(data + 10U);
    result.gyro_z = decode_i16_be(data + 12U);
    *sample = result;
    return MPU6050_OK;
}
