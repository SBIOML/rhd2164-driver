/** @file rhd.h
 *
 * @brief RHD C Driver header file
 *
 * @par
 * COPYRIGHT NOTICE: (c) 2023 SBIOML. All rights reserved.
 */

#ifndef CFFIRW_H
#define CFFIRW_H

#include <stddef.h>
#include <stdint.h>

// CFFI START

int my_rhd_rw(uint16_t *tx_buf, uint16_t *rx_buf, size_t len);

// CFFI END

#endif // CFFIRW_H