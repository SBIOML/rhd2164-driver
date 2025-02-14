/** @file rhd.c
 *
 * @brief Platform-agnostic RHD2000 driver. Mostly aimed at RHD2164, but should
 * work on RHD2000.
 *
 * COPYRIGHT NOTICE: (c) 2023 SBIOML.  All rights reserved.
 */

#include "rhd.h"

/**
 * @brief Duplicate the bits of a value.
 * It is assumed to be an 8-bits value.
 * For example, `0b0101 0011` becomes `0b0011 0011 0000 1111`
 *
 * @param val 8-bit value to double every bit
 * @return int the 16-bit value with duplicate bits.
 */
static int rhd_duplicate_bits(uint8_t val);

/**
 * @brief Unsplit SPI DDR flip-flopped data
 *
 * @param data source data as 0bxyxy xyxy xyxy xyxy
 * @param a destination 8-bit data as 0bxxxx xxxx
 * @param b destination 8-bit data as 0byyyy yyyy
 */
static void rhd_unsplit_u16(uint16_t data, uint8_t *a, uint8_t *b);

static const uint16_t RHD_ADC_CH_CMD_DOUBLE[32] = {
    0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F, 0xC0, 0xC3, 0xCC,
    0xCF, 0xF0, 0xF3, 0xFC, 0xFF, 0x300, 0x303, 0x30C, 0x30F, 0x330, 0x333,
    0x33C, 0x33F, 0x3C0, 0x3C3, 0x3CC, 0x3CF, 0x3F0, 0x3F3, 0x3FC, 0x3FF};
static const uint16_t RHD_ADC_CH_CMD[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                            11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                                            22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

uint8_t rhd_send(rhd_device_t *dev, uint16_t reg, uint16_t val)
{
  switch ((int)dev->double_bits)
  {
  case 0:
  {
    uint16_t tx = (reg << 8) | (val & 0xFF);
    uint16_t rx = 0;
    dev->rw(&tx, &rx, 1);
    return (uint8_t)(rx & 0xFF);
  }
  default:
  {
    uint16_t tx[2] = {0};
    uint16_t rx[2] = {0};
    tx[0] = rhd_duplicate_bits(reg);
    tx[1] = rhd_duplicate_bits(val);
    dev->rw(tx, rx, 2);
    uint8_t rx_a, rx_b;
    rhd_unsplit_u16(rx[1], &rx_a, &rx_b);
    return rx_a;
  }
  }
}

uint8_t rhd_r(rhd_device_t *dev, uint16_t reg)
{
  // reg is 6 bits, b[7,6] = [1, 1]
  reg = (reg & 0x3F) | 0xC0;
  return rhd_send(dev, reg, 0);
}

uint8_t rhd_w(rhd_device_t *dev, uint16_t reg, uint16_t val)
{
  // reg is 6 bits, b[7,6] = [1, 0]
  reg = (reg & 0x3F) | 0x80;
  return rhd_send(dev, reg, val);
}

int rhd_init(rhd_device_t *dev, bool mode, rhd_rw_t rw)
{
  dev->double_bits = mode;
  dev->rw = rw;
  return rhd_sanity_check(dev);
}

int rhd_setup(rhd_device_t *dev, float fs, float fl, float fh, bool dsp,
              float fdsp)
{
  // R0 : 1.225V Vref = 1, ADC comp bias = 3, ADC comp sel = 2
  // R4 : [b6] twoscomp = 1
  // High bandwidth (R8-R11) = 300 Hz
  // Low bandwifth (R12-R13) = 20 Hz

  // dummy cmds
  rhd_r(dev, CHIP_ID);
  rhd_r(dev, CHIP_ID);

  // configure everything
  rhd_w(dev, ADC_CFG, 0b11011110);
  rhd_w(dev, MUX_LOAD_TEMP_SENS_AUX_DIG_OUT, 0b00000000);
  // TODO fn to cfg temp/digout ^
  rhd_w(dev, IMP_CHK_CTRL, 0);
  rhd_w(dev, IMP_CHK_DAC, 0);
  rhd_w(dev, IMP_CHK_AMP_SEL, 0);

  rhd_cfg_fs(dev, fs, 32);
  rhd_cfg_dsp(dev, true, false, dsp, fdsp, fs);
  rhd_cfg_ch(dev, 0xFFFFFFFF, 0xFFFFFFFF);
  rhd_cfg_amp_bw(dev, fl, fh);

  rhd_calib(dev);

  return rhd_sanity_check(dev);
}

int rhd_cfg_ch(rhd_device_t *dev, uint32_t channels_l, uint32_t channels_h)
{
  rhd_w(dev, IND_AMP_PWR_0, channels_l & 0xFF);
  rhd_w(dev, IND_AMP_PWR_1, (channels_l >> 8) & 0xFF);
  rhd_w(dev, IND_AMP_PWR_2, (channels_l >> 16) & 0xFF);
  rhd_w(dev, IND_AMP_PWR_3, (channels_l >> 24) & 0xFF);

  rhd_w(dev, IND_AMP_PWR_4, channels_h & 0xFF);
  rhd_w(dev, IND_AMP_PWR_5, (channels_h >> 8) & 0xFF);
  rhd_w(dev, IND_AMP_PWR_6, (channels_h >> 16) & 0xFF);
  return rhd_w(dev, IND_AMP_PWR_7, (channels_h >> 24) & 0xFF);
}

int rhd_cfg_fs(rhd_device_t *dev, float fs, int n_ch)
{
  const float msps = fs * n_ch;
  const int msps_lut[9] = {120000, 140000, 175000, 220000, 280000,
                           350000, 440000, 525000, 700000};
  const int adc_buf_bias_lut[9] = {32, 16, 8, 8, 8, 4, 3, 3, 2};
  const int mux_bias_lut[9] = {40, 40, 40, 32, 26, 18, 16, 7, 4};

  int i_lut = 0;
  for (unsigned int i = 0; i < sizeof(msps_lut) / sizeof(int); i++)
  {
    if (msps <= msps_lut[i])
    {
      break;
    }
    i_lut = i;
  }

  rhd_w(dev, SUPPLY_SENS_ADC_BUF_BIAS, adc_buf_bias_lut[i_lut]);
  rhd_w(dev, MUX_BIAS_CURR, mux_bias_lut[i_lut]);

  return msps;
}

int rhd_cfg_amp_bw(rhd_device_t *dev, float fl, float fh)
{
  const int fh_lut[17] = {20000, 15000, 10000, 7500, 5000, 3000,
                          2500, 2000, 1500, 1000, 750, 500,
                          300, 250, 200, 150, 100};
  const int rh1_dac1_lut[17] = {8, 11, 17, 22, 33, 3, 13, 27, 1,
                                46, 41, 30, 6, 42, 24, 44, 38};
  const int rh1_dac2_lut[17] = {0, 0, 0, 0, 0, 1, 1, 1, 2,
                                2, 3, 5, 9, 10, 13, 17, 26};
  const int rh2_dac1_lut[17] = {4, 8, 16, 23, 37, 13, 25, 44, 23,
                                30, 36, 43, 2, 5, 7, 8, 5};
  const int rh2_dac2_lut[17] = {0, 0, 0, 0, 0, 1, 1, 1, 2,
                                3, 4, 6, 11, 13, 16, 21, 31};

  int i_fh = 0;
  for (unsigned int i = 0; i < sizeof(fh_lut) / sizeof(int); i++)
  {
    if (fh >= fh_lut[i])
    {
      break;
    }
    i_fh++;
  }

  const float fl_lut[25] = {0.1, 0.25, 0.3, 0.5, 0.75, 1.0, 1.5, 2.0, 2.5,
                            3.0, 5.0, 7.5, 10, 15, 20, 25, 30, 50,
                            75, 100, 150, 200, 250, 300, 500};
  const int rl_dac1_lut[25] = {16, 56, 1, 35, 49, 44, 9, 8, 42,
                               20, 40, 18, 5, 62, 54, 48, 44, 34,
                               28, 25, 21, 18, 17, 15, 13};
  const int rl_dac2_lut[25] = {60, 54, 40, 17, 9, 6, 4, 3, 2, 2, 1, 1, 1,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const int rl_dac3_lut[25] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  int i_fl = 0;
  for (unsigned int i = 0; i < sizeof(fl_lut) / sizeof(float); i++)
  {
    if (fl <= fl_lut[i])
    {
      break;
    }
    i_fl++;
  }

  rhd_w(dev, AMP_BW_SEL_0, rh1_dac1_lut[i_fh]);
  rhd_w(dev, AMP_BW_SEL_1, rh1_dac2_lut[i_fh]);
  rhd_w(dev, AMP_BW_SEL_2, rh2_dac1_lut[i_fh]);
  rhd_w(dev, AMP_BW_SEL_3, rh2_dac2_lut[i_fh]);
  rhd_w(dev, AMP_BW_SEL_4, rl_dac1_lut[i_fl]);
  int ret =
      rhd_w(dev, AMP_BW_SEL_5, (rl_dac3_lut[i_fl] << 6) | rl_dac2_lut[i_fl]);

  return ret;
}

int rhd_cfg_dsp(rhd_device_t *dev, bool twos_comp, bool abs_mode, bool dsp,
                float fdsp, float fs)
{
  const double k_lut[16] = {0.99, 0.1103, 0.04579, 0.02125,
                            0.01027, 0.005053, 0.002506, 0.001248,
                            0.0006229, 0.0003112, 0.0001555, 0.00007773,
                            0.00003886, 0.00001943, 0.000009714, 0.000004857};

  int dsp_val = 0;
  if (dsp)
  {
    float k = fdsp / fs;
    for (unsigned int i = 0; i < sizeof(k_lut) / sizeof(double); i++)
    {
      if (k > k_lut[i])
      {
        break;
      }
      dsp_val++;
    }
  }

  return rhd_w(dev, ADC_OUT_FMT_DPS_OFF_RMVL,
               (1 << 7) | (((int)twos_comp) << 6) | (((int)abs_mode) << 5) |
                   (((int)dsp) << 4) | dsp_val);
}

uint8_t rhd_calib(rhd_device_t *dev)
{
  int ret = rhd_send(dev, 0b01010101, 0);

  for (int i = 0; i < 9; i++)
  {
    // 9 dummy cmds
    ret = rhd_r(dev, CHIP_ID);
  }

  return ret;
}

uint8_t rhd_clear_calib(rhd_device_t *dev) { return rhd_send(dev, 0b01101010, 0); }

int rhd_sanity_check(rhd_device_t *dev)
{
  const char INTAN[] = "INTAN";
  int ret = 0;
  for (int i = 0; i < sizeof(INTAN) - 1; i++)
  {
    char let = (char)rhd_read_force(dev, INTAN_0 + i);
    if (let != INTAN[i])
    {
      ret = i + INTAN_0;
      break;
    }
  }
  return ret;
}

uint8_t rhd_read_force(rhd_device_t *dev, int reg)
{
  for (int i = 0; i < 2; i++)
  {
    rhd_r(dev, reg);
  }
  return rhd_r(dev, reg);
}

uint16_t *rhd2164_sample(rhd_device_t *dev, uint16_t ch, uint16_t *rx)
{
  switch ((int)dev->double_bits)
  {
  case 0:
  {
    uint16_t tx = (ch << 8);
    dev->rw(&tx, rx, 1);
    return rx;
  }
  default:
  {
    uint16_t tx[2] = {0};
    tx[0] = rhd_duplicate_bits(ch);

    dev->rw(tx, rx, 2);

    uint8_t dat_a[2] = {0};
    uint8_t dat_b[2] = {0};
    rhd_unsplit_u16(rx[0], &dat_a[1], &dat_b[1]);
    rhd_unsplit_u16(rx[1], &dat_a[0], &dat_b[0]);
    rx[0] =
        (((uint16_t)dat_a[1]) << 8) | dat_a[0] | 1;
    rx[1] =
        (((uint16_t)dat_b[1]) << 8) | dat_b[0] | 1;
    return rx;
  }
  }
}

uint16_t rhd2000_sample(rhd_device_t *dev, uint16_t ch)
{
  uint16_t tx = (ch << 8);
  uint16_t rx;
  dev->rw(&tx, &rx, 1);
  return rx;
}

void rhd2164_sample_all(rhd_device_t *dev, uint16_t *sample_buf)
{
  // Let ch0 sample from last iter, ask for ch1
  // const uint16_t *RHD_ADC_CH =
  //     (int)dev->double_bits ? RHD_ADC_CH_CMD_DOUBLE : RHD_ADC_CH_CMD;
  uint16_t rx[2] = {0};

  for (int ch = 0; ch < 32; ch++)
  {
    rhd2164_sample(dev, ch, rx);
    int rx_ch = ch < 2 ? 31 - ch : ch - 2;
    sample_buf[rx_ch] = rx[0];
    sample_buf[rx_ch + 32] = rx[1];
  }
  // Alignment
  sample_buf[0] &= 0xFFFE;
}

static int rhd_duplicate_bits(uint8_t val)
{
  int out = 0;
  for (int i = 0; i < 8; i++)
  {
    int tmp = (val >> i) & 1;
    out |= (tmp << 1 | tmp) << 2 * i;
  }
  return out;
}

static void rhd_unsplit_u16(uint16_t data, uint8_t *a, uint8_t *b)
{
  static const uint8_t shift_arr[] = {0x1, 0x2, 0x4, 0x8,
                                      0x10, 0x20, 0x40, 0x80};

  uint8_t aa = 0;
  uint8_t bb = 0;

  uint16_t dataa = data >> 1;
  // Doubt we can optimize it much more
  for (int i = 0; i < 8; i++)
  {
    aa |= (dataa >> i) & shift_arr[i];
    bb |= (data >> i) & shift_arr[i];
  }
  *a = aa;
  *b = bb;
}
