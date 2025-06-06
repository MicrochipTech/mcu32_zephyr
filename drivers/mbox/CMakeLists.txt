# SPDX-License-Identifier: Apache-2.0

zephyr_syscall_header(${ZEPHYR_BASE}/include/zephyr/drivers/mbox.h)

zephyr_library()

zephyr_library_sources_ifdef(CONFIG_USERSPACE   mbox_handlers.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRFX_IPC   mbox_nrfx_ipc.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NXP_S32_MRU   mbox_nxp_s32_mru.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NXP_IMX_MU   mbox_nxp_imx_mu.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NXP_MAILBOX   mbox_nxp_mailbox.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_ESP32   mbox_esp32.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_ANDES_PLIC_SW   mbox_andes_plic_sw.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_VEVIF_TASK_RX mbox_nrf_vevif_task_rx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_VEVIF_TASK_TX mbox_nrf_vevif_task_tx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_VEVIF_EVENT_RX mbox_nrf_vevif_event_rx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_VEVIF_EVENT_TX mbox_nrf_vevif_event_tx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_BELLBOARD_RX mbox_nrf_bellboard_rx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_NRF_BELLBOARD_TX mbox_nrf_bellboard_tx.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_STM32_HSEM   mbox_stm32_hsem.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_IVSHMEM mbox_ivshmem.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_TI_OMAP_MAILBOX   mbox_ti_omap.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_RENESAS_RZ_MHU mbox_renesas_rz_mhu.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_MHUV3   mbox_mhuv3.c)
zephyr_library_sources_ifdef(CONFIG_MBOX_TI_SECURE_PROXY   mbox_ti_secproxy.c)
