#include "config.h"
#include "mpu6050_port.h"

#if MPU6050_BOARD_CONFIGURED
#include "stm32f10x.h"
#include "system_time.h"

#define I2C_ERROR_FLAGS (I2C_FLAG_BERR | I2C_FLAG_ARLO | I2C_FLAG_AF | \
                         I2C_FLAG_OVR | I2C_FLAG_PECERR | I2C_FLAG_TIMEOUT)

static uint8_t bus_fault;

static MPU6050_Status_t wait_bus_idle(void)
{
    uint32_t start = SystemTime_GetMs();
    uint32_t spins;
    for (spins = 0U; spins < MPU6050_I2C_SPIN_LIMIT; ++spins)
    {
        if (I2C_GetFlagStatus(MPU6050_I2C, I2C_FLAG_BUSY) == RESET)
        {
            return MPU6050_OK;
        }
        if ((uint32_t)(SystemTime_GetMs() - start) >= MPU6050_I2C_TIMEOUT_MS)
        {
            return MPU6050_TIMEOUT;
        }
    }
    return MPU6050_TIMEOUT;
}

static MPU6050_Status_t wait_event(uint32_t event)
{
    uint32_t start = SystemTime_GetMs();
    uint32_t spins;
    for (spins = 0U; spins < MPU6050_I2C_SPIN_LIMIT; ++spins)
    {
        if ((MPU6050_I2C->SR1 & I2C_ERROR_FLAGS) != 0U)
        {
            return MPU6050_ERROR;
        }
        if (I2C_CheckEvent(MPU6050_I2C, event) == SUCCESS)
        {
            return MPU6050_OK;
        }
        if ((uint32_t)(SystemTime_GetMs() - start) >= MPU6050_I2C_TIMEOUT_MS)
        {
            return MPU6050_TIMEOUT;
        }
    }
    return MPU6050_TIMEOUT;
}

static MPU6050_Status_t wait_rxne(void)
{
    uint32_t start = SystemTime_GetMs();
    uint32_t spins;
    for (spins = 0U; spins < MPU6050_I2C_SPIN_LIMIT; ++spins)
    {
        if ((MPU6050_I2C->SR1 & I2C_ERROR_FLAGS) != 0U)
        {
            return MPU6050_ERROR;
        }
        if (I2C_GetFlagStatus(MPU6050_I2C, I2C_FLAG_RXNE) == SET)
        {
            return MPU6050_OK;
        }
        if ((uint32_t)(SystemTime_GetMs() - start) >= MPU6050_I2C_TIMEOUT_MS)
        {
            return MPU6050_TIMEOUT;
        }
    }
    return MPU6050_TIMEOUT;
}

static MPU6050_Status_t abort_transaction(MPU6050_Status_t status)
{
    I2C_GenerateSTOP(MPU6050_I2C, ENABLE);
    I2C_AcknowledgeConfig(MPU6050_I2C, ENABLE);
    I2C_Cmd(MPU6050_I2C, DISABLE);
    bus_fault = 1U;
    return status;
}

MPU6050_Status_t MPU6050_PortInit(void)
{
    GPIO_InitTypeDef gpio;
    I2C_InitTypeDef i2c;
    MPU6050_Status_t status;
    if (MPU6050_I2C != I2C2) { return MPU6050_INVALID_PARAM; }
    if ((MPU6050_I2C_CLOCK_HZ == 0U) || (MPU6050_I2C_CLOCK_HZ > 400000UL))
    {
        return MPU6050_INVALID_PARAM;
    }
    RCC_APB2PeriphClockCmd(MPU6050_GPIO_CLOCKS, ENABLE);
    MPU6050_I2C_CLOCK_ENABLE();
    MPU6050_I2C_FORCE_RESET();
    MPU6050_I2C_RELEASE_RESET();

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = MPU6050_SCL_PIN | MPU6050_SDA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AF_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(MPU6050_GPIO_PORT, &gpio);

    I2C_StructInit(&i2c);
    i2c.I2C_Mode = I2C_Mode_I2C;
    i2c.I2C_DutyCycle = I2C_DutyCycle_2;
    i2c.I2C_OwnAddress1 = 0U;
    i2c.I2C_Ack = I2C_Ack_Enable;
    i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    i2c.I2C_ClockSpeed = MPU6050_I2C_CLOCK_HZ;
    I2C_Init(MPU6050_I2C, &i2c);
    I2C_AcknowledgeConfig(MPU6050_I2C, ENABLE);
    I2C_Cmd(MPU6050_I2C, ENABLE);
    bus_fault = 0U;
    status = wait_bus_idle();
    if (status != MPU6050_OK) { return abort_transaction(status); }
    return MPU6050_OK;
}

MPU6050_Status_t MPU6050_PortWriteRegister(uint8_t address, uint8_t value)
{
    MPU6050_Status_t status;
    if (bus_fault) { return MPU6050_ERROR; }
    status = wait_bus_idle();
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_GenerateSTART(MPU6050_I2C, ENABLE);
    status = wait_event(I2C_EVENT_MASTER_MODE_SELECT);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_Send7bitAddress(MPU6050_I2C,
                        (uint8_t)(MPU6050_I2C_ADDRESS_7BIT << 1U),
                        I2C_Direction_Transmitter);
    status = wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_SendData(MPU6050_I2C, address);
    status = wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_SendData(MPU6050_I2C, value);
    status = wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_GenerateSTOP(MPU6050_I2C, ENABLE);
    return MPU6050_OK;
}

MPU6050_Status_t MPU6050_PortReadRegisters(uint8_t address,
                                           uint8_t *data,
                                           uint32_t length)
{
    MPU6050_Status_t status;
    uint32_t remaining;
    if ((data == 0) || (length == 0U) ||
        (length > 256UL - (uint32_t)address))
    {
        return MPU6050_INVALID_PARAM;
    }
    if (bus_fault) { return MPU6050_ERROR; }
    status = wait_bus_idle();
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_GenerateSTART(MPU6050_I2C, ENABLE);
    status = wait_event(I2C_EVENT_MASTER_MODE_SELECT);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_Send7bitAddress(MPU6050_I2C,
                        (uint8_t)(MPU6050_I2C_ADDRESS_7BIT << 1U),
                        I2C_Direction_Transmitter);
    status = wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_SendData(MPU6050_I2C, address);
    status = wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_GenerateSTART(MPU6050_I2C, ENABLE);
    status = wait_event(I2C_EVENT_MASTER_MODE_SELECT);
    if (status != MPU6050_OK) { return abort_transaction(status); }
    I2C_Send7bitAddress(MPU6050_I2C,
                        (uint8_t)(MPU6050_I2C_ADDRESS_7BIT << 1U),
                        I2C_Direction_Receiver);
    status = wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);
    if (status != MPU6050_OK) { return abort_transaction(status); }

    remaining = length;
    while (remaining > 0U)
    {
        if (remaining == 1U)
        {
            I2C_AcknowledgeConfig(MPU6050_I2C, DISABLE);
            I2C_GenerateSTOP(MPU6050_I2C, ENABLE);
        }
        status = wait_rxne();
        if (status != MPU6050_OK) { return abort_transaction(status); }
        *data = (uint8_t)I2C_ReceiveData(MPU6050_I2C);
        ++data;
        --remaining;
    }
    I2C_AcknowledgeConfig(MPU6050_I2C, ENABLE);
    return MPU6050_OK;
}

#else
MPU6050_Status_t MPU6050_PortInit(void) { return MPU6050_NOT_CONFIGURED; }
MPU6050_Status_t MPU6050_PortWriteRegister(uint8_t address, uint8_t value)
{
    (void)address;
    (void)value;
    return MPU6050_NOT_CONFIGURED;
}
MPU6050_Status_t MPU6050_PortReadRegisters(uint8_t address,
                                           uint8_t *data,
                                           uint32_t length)
{
    (void)address;
    (void)data;
    (void)length;
    return MPU6050_NOT_CONFIGURED;
}
#endif
