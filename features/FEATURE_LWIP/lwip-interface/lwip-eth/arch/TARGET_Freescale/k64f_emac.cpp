/*
 * Copyright (c) 2013 - 2014, Freescale Semiconductor, Inc.
 * Copyright (c) 2017 ARM Limited
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of Freescale Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cmsis_os.h"

#include "mbed_interface.h"
#include "emac_stack_mem.h"
#include "mbed_assert.h"
#include "netsocket/nsapi_types.h"
#include "mbed_shared_queues.h"

#if DEVICE_EMAC

#include "fsl_phy.h"

#include "k64f_emac_config.h"
#include "k64f_emac.h"

enet_handle_t g_handle;
// TX Buffer descriptors
uint8_t *tx_desc_start_addr;
// RX Buffer descriptors
uint8_t *rx_desc_start_addr;
// RX packet buffer pointers
emac_stack_mem_t *rx_buff[ENET_RX_RING_LEN];
// TX packet buffer pointers
emac_stack_mem_t *tx_buff[ENET_RX_RING_LEN];
// RX packet payload pointers
uint32_t *rx_ptr[ENET_RX_RING_LEN];

/********************************************************************************
 * Internal data
 ********************************************************************************/
#define ENET_BuffSizeAlign(n) ENET_ALIGN(n, ENET_BUFF_ALIGNMENT)
#define ENET_ALIGN(x,align)   ((unsigned int)((x) + ((align)-1)) & (unsigned int)(~(unsigned int)((align)- 1)))
#if (defined(TARGET_K64F) && (defined(TARGET_FRDM)))
extern "C" void k64f_init_eth_hardware(void);
#endif

#if (defined(TARGET_K66F) && (defined(TARGET_FRDM)))
extern "C" void k66f_init_eth_hardware(void);
#endif

/* \brief Flags for worker thread */
#define FLAG_TX  1
#define FLAG_RX  2

/** \brief  Driver thread priority */
#define THREAD_PRIORITY (osPriorityNormal)

#define PHY_TASK_PERIOD_MS      200

K64F_EMAC::K64F_EMAC() : xTXDCountSem(ENET_TX_RING_LEN, ENET_TX_RING_LEN), hwaddr()
{
}

static osThreadId_t create_new_thread(const char *threadName, void (*thread)(void *arg), void *arg, int stacksize, osPriority_t priority, os_thread_t *thread_cb)
{
    osThreadAttr_t attr = {0};
    attr.name = threadName;
    attr.stack_mem  = malloc(stacksize);
    attr.cb_mem  = thread_cb;
    attr.stack_size = stacksize;
    attr.cb_size = sizeof(os_thread_t);
    attr.priority = priority;
    return osThreadNew(thread, arg, &attr);
}
/********************************************************************************
 * Buffer management
 ********************************************************************************/
/*
 * This function will queue a new receive buffer
 */
static void update_read_buffer(uint8_t *buf)
{
    if (buf != NULL) {
        g_handle.rxBdCurrent->buffer = buf;
    }

    /* Clears status. */
    g_handle.rxBdCurrent->control &= ENET_BUFFDESCRIPTOR_RX_WRAP_MASK;

    /* Sets the receive buffer descriptor with the empty flag. */
    g_handle.rxBdCurrent->control |= ENET_BUFFDESCRIPTOR_RX_EMPTY_MASK;

    /* Increases the buffer descriptor to the next one. */
    if (g_handle.rxBdCurrent->control & ENET_BUFFDESCRIPTOR_RX_WRAP_MASK) {
        g_handle.rxBdCurrent = g_handle.rxBdBase;
    } else {
        g_handle.rxBdCurrent++;
    }

    /* Actives the receive buffer descriptor. */
    ENET->RDAR = ENET_RDAR_RDAR_MASK;
}

/** \brief  Free TX buffers that are complete
 */
void K64F_EMAC::tx_reclaim()
{
  /* Get exclusive access */
  TXLockMutex.lock();

  // Traverse all descriptors, looking for the ones modified by the uDMA
  while((tx_consume_index != tx_produce_index) &&
        (!(g_handle.txBdDirty->control & ENET_BUFFDESCRIPTOR_TX_READY_MASK))) {
      emac_stack_mem_free(tx_buff[tx_consume_index % ENET_TX_RING_LEN]);
      if (g_handle.txBdDirty->control & ENET_BUFFDESCRIPTOR_TX_WRAP_MASK)
        g_handle.txBdDirty = g_handle.txBdBase;
      else
        g_handle.txBdDirty++;

      tx_consume_index += 1;
      xTXDCountSem.release();
  }

  /* Restore access */
  TXLockMutex.unlock();
}

/** \brief Ethernet receive interrupt handler
 *
 *  This function handles the receive interrupt of K64F.
 */
void K64F_EMAC::rx_isr()
{
    if (thread) {
        osThreadFlagsSet(thread, FLAG_RX);
    }
}

void K64F_EMAC::tx_isr()
{
    osThreadFlagsSet(thread, FLAG_TX);
}

void K64F_EMAC::ethernet_callback(ENET_Type *base, enet_handle_t *handle, enet_event_t event, void *param)
{
    K64F_EMAC *enet = static_cast<K64F_EMAC *>(param);
    switch (event)
    {
      case kENET_RxEvent:
        enet->rx_isr();
        break;
      case kENET_TxEvent:
        enet->tx_isr();
        break;
      default:
        break;
    }
}


/** \brief  Low level init of the MAC and PHY.
 */
bool K64F_EMAC::low_level_init_successful()
{
  uint8_t i;
  uint32_t sysClock;
  phy_speed_t phy_speed;
  phy_duplex_t phy_duplex;
  uint32_t phyAddr = 0;
  bool link = false;
  enet_config_t config;

  // Allocate RX descriptors
  rx_desc_start_addr = (uint8_t *)calloc(1, sizeof(enet_rx_bd_struct_t) * ENET_RX_RING_LEN + ENET_BUFF_ALIGNMENT);
  if(!rx_desc_start_addr)
    return false;

  // Allocate TX descriptors
  tx_desc_start_addr = (uint8_t *)calloc(1, sizeof(enet_tx_bd_struct_t) * ENET_TX_RING_LEN + ENET_BUFF_ALIGNMENT);
  if(!tx_desc_start_addr)
    return false;

  rx_desc_start_addr = (uint8_t *)ENET_ALIGN(rx_desc_start_addr, ENET_BUFF_ALIGNMENT);
  tx_desc_start_addr = (uint8_t *)ENET_ALIGN(tx_desc_start_addr, ENET_BUFF_ALIGNMENT);

  /* Create buffers for each receive BD */
  for (i = 0; i < ENET_RX_RING_LEN; i++) {
    rx_buff[i] = emac_stack_mem_alloc(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT);
    if (NULL == rx_buff[i])
      return false;

    rx_ptr[i] = (uint32_t*)emac_stack_mem_ptr(rx_buff[i]);
  }

  tx_consume_index = tx_produce_index = 0;

  /* prepare the buffer configuration. */
  enet_buffer_config_t buffCfg = {
    ENET_RX_RING_LEN,
    ENET_TX_RING_LEN,
    ENET_ALIGN(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT),
    0,
    (volatile enet_rx_bd_struct_t *)rx_desc_start_addr,
    (volatile enet_tx_bd_struct_t *)tx_desc_start_addr,
    (uint8_t *)&rx_ptr,
    NULL,
  };
#if (defined(TARGET_K64F) && (defined(TARGET_FRDM)))
  k64f_init_eth_hardware();
#endif

#if (defined(TARGET_K66F) && (defined(TARGET_FRDM)))
  k66f_init_eth_hardware();
#endif

  sysClock = CLOCK_GetFreq(kCLOCK_CoreSysClk);

  ENET_GetDefaultConfig(&config);

  PHY_Init(ENET, 0, sysClock);
  PHY_GetLinkStatus(ENET, phyAddr, &link);
  if (link)
  {
    /* Get link information from PHY */
    PHY_GetLinkSpeedDuplex(ENET, phyAddr, &phy_speed, &phy_duplex);
    /* Change the MII speed and duplex for actual link status. */
    config.miiSpeed = (enet_mii_speed_t)phy_speed;
    config.miiDuplex = (enet_mii_duplex_t)phy_duplex;
    config.interrupt = kENET_RxFrameInterrupt | kENET_TxFrameInterrupt;
  }
  config.rxMaxFrameLen = ENET_ETH_MAX_FLEN;
  config.macSpecialConfig = kENET_ControlFlowControlEnable;
  config.txAccelerConfig = 0;
  config.rxAccelerConfig = kENET_RxAccelMacCheckEnabled;
  ENET_Init(ENET, &g_handle, &config, &buffCfg, hwaddr, sysClock);

#if defined(TOOLCHAIN_ARM)
#if defined(__OPTIMISE_TIME) && (__ARMCC_VERSION < 5060750)
  /* Add multicast groups
     work around for https://github.com/ARMmbed/mbed-os/issues/4372 */
  ENET->GAUR = 0xFFFFFFFFu;
  ENET->GALR = 0xFFFFFFFFu;
#endif
#endif

  ENET_SetCallback(&g_handle, &K64F_EMAC::ethernet_callback, this);
  ENET_ActiveRead(ENET);

  return true;
}


/** \brief  Allocates a emac_stack_mem_t and returns the data from the incoming packet.
 *
 *  \param[in] idx   index of packet to be read
 *  \return a emac_stack_mem_t filled with the received packet (including MAC header)
 */
emac_stack_mem_t *K64F_EMAC::low_level_input(int idx)
{
  volatile enet_rx_bd_struct_t *bdPtr = g_handle.rxBdCurrent;
  emac_stack_mem_t *p = NULL;
  emac_stack_mem_t *temp_rxbuf = NULL;
  uint32_t length = 0;
  const uint16_t err_mask = ENET_BUFFDESCRIPTOR_RX_TRUNC_MASK | ENET_BUFFDESCRIPTOR_RX_CRC_MASK |
                            ENET_BUFFDESCRIPTOR_RX_NOOCTET_MASK | ENET_BUFFDESCRIPTOR_RX_LENVLIOLATE_MASK;

#ifdef LOCK_RX_THREAD
  /* Get exclusive access */
  TXLockMutex.lock();
#endif

  /* Determine if a frame has been received */
  if ((bdPtr->control & err_mask) != 0) {
    /* Re-use the same buffer in case of error */
    update_read_buffer(NULL);
  } else {
    /* A packet is waiting, get length */
    length = bdPtr->length;

    /* Zero-copy */
    p = rx_buff[idx];
    emac_stack_mem_set_len(p, length);

    /* Attempt to queue new buffer */
    temp_rxbuf = emac_stack_mem_alloc(ENET_ETH_MAX_FLEN, ENET_BUFF_ALIGNMENT);
    if (NULL == temp_rxbuf) {
      /* Re-queue the same buffer */
      update_read_buffer(NULL);

#ifdef LOCK_RX_THREAD
      TXLockMutex.unlock();
#endif

      return NULL;
    }

    rx_buff[idx] = temp_rxbuf;
    rx_ptr[idx] = (uint32_t*)emac_stack_mem_ptr(rx_buff[idx]);

    update_read_buffer((uint8_t*)rx_ptr[idx]);

    /* Save size */
    emac_stack_mem_set_chain_len(p, length);
  }

#ifdef LOCK_RX_THREAD
  osMutexRelease(TXLockMutex);
#endif

  return p;
}

/** \brief  Attempt to read a packet from the EMAC interface.
 *
 *  \param[in] idx   index of packet to be read
 */
void K64F_EMAC::input(int idx)
{
  emac_stack_mem_t *p;

  /* move received packet into a new buf */
  p = low_level_input(idx);
  if (p == NULL)
    return;

  emac_link_input_cb(p);

}

/** \brief  Worker thread.
 *
 * Woken by thread flags to receive packets or clean up transmit
 *
 *  \param[in] pvParameters pointer to the interface data
 */
void K64F_EMAC::thread_function(void* pvParameters)
{
    struct K64F_EMAC *k64f_enet = static_cast<K64F_EMAC *>(pvParameters);

    for (;;) {
        uint32_t flags = osThreadFlagsWait(FLAG_RX|FLAG_TX, osFlagsWaitAny, osWaitForever);

        MBED_ASSERT(!(flags & osFlagsError));

        if (flags & FLAG_RX) {
            k64f_enet->packet_rx();
        }

        if (flags & FLAG_TX) {
            k64f_enet->packet_tx();
        }
    }
}

/** \brief  Packet reception task
 *
 * This task is called when a packet is received. It will
 * pass the packet to the LWIP core.
 */
void K64F_EMAC::packet_rx()
{
    static int idx = 0;

    while ((g_handle.rxBdCurrent->control & ENET_BUFFDESCRIPTOR_RX_EMPTY_MASK) == 0) {
        input(idx);
        idx = (idx + 1) % ENET_RX_RING_LEN;
    }
}

/** \brief  Transmit cleanup task
 *
 * This task is called when a transmit interrupt occurs and
 * reclaims the buffer and descriptor used for the packet once
 * the packet has been transferred.
 */
void K64F_EMAC::packet_tx()
{
    tx_reclaim();
}

/** \brief  Low level output of a packet. Never call this from an
 *          interrupt context, as it may block until TX descriptors
 *          become available.
 *
 *  \param[in] buf      the MAC packet to send (e.g. IP packet including MAC addresses and type)
 *  \return ERR_OK if the packet could be sent or an err_t value if the packet couldn't be sent
 */
bool K64F_EMAC::link_out(emac_stack_mem_chain_t *chain)
{
  emac_stack_mem_t *q;
  emac_stack_mem_t *temp_pbuf;
  uint8_t *psend = NULL, *dst;

  temp_pbuf = emac_stack_mem_alloc(emac_stack_mem_chain_len(chain), ENET_BUFF_ALIGNMENT);
  if (NULL == temp_pbuf)
    return false;

  psend = (uint8_t*)emac_stack_mem_ptr(temp_pbuf);
  for (q = emac_stack_mem_chain_dequeue(&chain), dst = psend; q != NULL; q = emac_stack_mem_chain_dequeue(&chain)) {
    memcpy(dst, emac_stack_mem_ptr(q), emac_stack_mem_len(q));
    dst += emac_stack_mem_len(q);
  }

  /* Check if a descriptor is available for the transfer. */
  if (xTXDCountSem.wait(0) == 0)
    return false;

  /* Get exclusive access */
  TXLockMutex.lock();

  /* Save the buffer so that it can be freed when transmit is done */
  tx_buff[tx_produce_index % ENET_TX_RING_LEN] = temp_pbuf;
  tx_produce_index += 1;

  /* Setup transfers */
  g_handle.txBdCurrent->buffer = psend;
  g_handle.txBdCurrent->length = emac_stack_mem_len(temp_pbuf);
  g_handle.txBdCurrent->control |= (ENET_BUFFDESCRIPTOR_TX_READY_MASK | ENET_BUFFDESCRIPTOR_TX_LAST_MASK);

  /* Increase the buffer descriptor address. */
  if (g_handle.txBdCurrent->control & ENET_BUFFDESCRIPTOR_TX_WRAP_MASK)
    g_handle.txBdCurrent = g_handle.txBdBase;
  else
    g_handle.txBdCurrent++;

  /* Active the transmit buffer descriptor. */
  ENET->TDAR = ENET_TDAR_TDAR_MASK;

  /* Restore access */
  TXLockMutex.unlock();

  return true;
}

/*******************************************************************************
 * PHY task: monitor link
*******************************************************************************/

#define STATE_UNKNOWN           (-1)

int phy_link_status(void) {
    bool connection_status;
    uint32_t phyAddr = 0;

    PHY_GetLinkStatus(ENET, phyAddr, &connection_status);
    return (int)connection_status;
}

void K64F_EMAC::phy_task()
{
    static PHY_STATE prev_state = {STATE_UNKNOWN, (phy_speed_t)STATE_UNKNOWN, (phy_duplex_t)STATE_UNKNOWN};

    uint32_t phyAddr = 0;

    // Get current status
    PHY_STATE crt_state;
    bool connection_status;
    PHY_GetLinkStatus(ENET, phyAddr, &connection_status);
    crt_state.connected = connection_status;
    // Get the actual PHY link speed
    PHY_GetLinkSpeedDuplex(ENET, phyAddr, &crt_state.speed, &crt_state.duplex);

    // Compare with previous state
    if (crt_state.connected != prev_state.connected) {
        emac_link_state_cb(crt_state.connected);
    }

    if (crt_state.speed != prev_state.speed) {
      uint32_t rcr = ENET->RCR;
      rcr &= ~ENET_RCR_RMII_10T_MASK;
      rcr |= ENET_RCR_RMII_10T(!crt_state.speed);
      ENET->RCR = rcr;
    }

    prev_state = crt_state;
}

bool K64F_EMAC::power_up()
{
  /* Initialize the hardware */
  if (!low_level_init_successful())
    return false;

  /* Worker thread */
  thread = create_new_thread("k64f_emac_thread", &K64F_EMAC::thread_function, this, THREAD_STACKSIZE, THREAD_PRIORITY, &thread_cb);

  /* Trigger thread to deal with any RX packets that arrived before thread was started */
  rx_isr();

  /* PHY monitoring task */
  prev_state.connected = STATE_UNKNOWN;
  prev_state.speed = (phy_speed_t)STATE_UNKNOWN;
  prev_state.duplex = (phy_duplex_t)STATE_UNKNOWN;

  phy_task_handle = mbed::mbed_event_queue()->call_every(PHY_TASK_PERIOD_MS, mbed::callback(this, &K64F_EMAC::phy_task));

  /* Allow the PHY task to detect the initial link state and set up the proper flags */
  osDelay(10);

  return true;
}


uint32_t K64F_EMAC::get_mtu_size() const
{
  return K64_ETH_MTU_SIZE;
}

void K64F_EMAC::get_ifname(char *name, uint8_t size) const
{
  memcpy(name, K64_ETH_IF_NAME, (size < sizeof(K64_ETH_IF_NAME)) ? size : sizeof(K64_ETH_IF_NAME));
}

uint8_t K64F_EMAC::get_hwaddr_size() const
{
    return K64F_HWADDR_SIZE;
}

bool K64F_EMAC::get_hwaddr(uint8_t *addr) const
{
  return false;
}

void K64F_EMAC::set_hwaddr(const uint8_t *addr)
{
  memcpy(hwaddr, addr, sizeof hwaddr);
  ENET_SetMacAddr(ENET, const_cast<uint8_t*>(addr));
}

void K64F_EMAC::set_link_input_cb(emac_link_input_cb_t input_cb)
{
  emac_link_input_cb = input_cb;
}

void K64F_EMAC::set_link_state_cb(emac_link_state_change_cb_t state_cb)
{
  emac_link_state_cb = state_cb;
}

void K64F_EMAC::add_multicast_group(uint8_t *addr)
{
  ENET_AddMulticastGroup(ENET, addr);
}

void K64F_EMAC::power_down()
{
  /* No-op at this stage */
}


K64F_EMAC &K64F_EMAC::get_instance() {
    static K64F_EMAC emac;
    return emac;
}

// Weak so a module can override
MBED_WEAK EMAC &EMAC::get_default_instance() {
    return K64F_EMAC::get_instance();
}

/**
 * @}
 */

#endif // DEVICE_EMAC

/* --------------------------------- End Of File ------------------------------ */

