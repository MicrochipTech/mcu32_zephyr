/*
 * Copyright (c) 2025 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief System control block context helpers for Cortex-M CPUs
 *
 * System control block context helpers for backup and restore
 */

#ifndef ARM_CORTEX_M_SCB_H_
#define ARM_CORTEX_M_SCB_H_

#include <stdint.h>
#include <cmsis_core.h>

/* Define macros for CPU-conditional features */
#if defined(CONFIG_CPU_CORTEX_M0)  || \
	defined(CONFIG_CPU_CORTEX_M0PLUS)  || \
	defined(CONFIG_CPU_CORTEX_M1) || \
	defined(CONFIG_CPU_CORTEX_M23)
#define SHPR_SIZE_W 2
#else
#define SHPR_SIZE_W 3
#define CPACR_PRESENT 1
#endif

/**
 * @brief Structure to store essential, mutable SCB register values for backup/restore.
 *
 * This structure explicitly lists the SCB registers that are safe and meaningful
 * to backup and restore for common system state management. It avoids volatile,
 * read-only, or write-only status bits that should not be directly restored.
 */
struct scb_context {
#if defined(CONFIG_CPU_CORTEX_M_HAS_VTOR)
	uint32_t vtor;   /*!< offset 0x08, Vector Table Offset Register */
#endif
	uint32_t aircr;  /*!< offset 0x0C, Application Interrupt and Reset Control Register
			  * (only modifiable bits)
			  */
	uint32_t scr;    /*!< offset 0x10, System Control Register */
	uint32_t ccr;    /*!< offset 0x14, Configuration Control Register */
	uint32_t shpr[SHPR_SIZE_W]; /*!< offset 0x18 or 0x1C, System Handler Priority Registers */
	uint32_t shcsr;  /*!< offset 0x24, System Handler Control and State Register */
#if defined(CPACR_PRESENT)
	uint32_t cpacr; /*!< offset 0x88, Coprocessor Access Control Register */
#endif /* CPACR_PRESENT */
};

/**
 * @name SCB Register Backup/Restore Functions
 * @brief Functions for saving and restoring mutable SCB register state.
 * @{
 */

/**
 * @brief Save essential SCB registers into a provided context structure.
 *
 * This function reads the current values of critical System Control Block (SCB)
 * registers that are safe to backup and stores them into the `context` structure.
 *
 * @param context Pointer to an `scb_context` structure where the register
 * values will be stored. Must not be NULL.
 */
void z_arm_save_scb_context(struct scb_context *context);

/**
 * @brief Restores essential SCB registers from a provided context structure.
 *
 * This function writes the values from the `context` structure back to the
 * respective System Control Block (SCB) registers.
 *
 * @warning Extreme caution is advised when restoring SCB registers. Only
 * mutable registers are restored. Specifically, the ICSR register
 * is NOT restored directly due to its volatile nature and read-only/
 * write-only bits.
 *
 * @param context Pointer to a `scb_context` structure containing the
 * register values to be restored. Must not be NULL.
 */
void z_arm_restore_scb_context(const struct scb_context *context);

/** @} */

#endif /* ARM_CORTEX_M_SCB_H_ */
