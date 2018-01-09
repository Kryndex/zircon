// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#if WITH_DEV_PCIE

#include <object/pci_interrupt_dispatcher.h>

#include <kernel/auto_lock.h>
#include <zircon/rights.h>
#include <fbl/alloc_checker.h>
#include <object/pci_device_dispatcher.h>
#include <platform.h>

PciInterruptDispatcher::~PciInterruptDispatcher() {
    if (device_) {
        // Unregister our handler.
        __UNUSED zx_status_t ret;
        ret = device_->RegisterIrqHandler(irq_id_, nullptr, nullptr);
        DEBUG_ASSERT(ret == ZX_OK);  // This should never fail.

        // Release our reference to our device.
        device_ = nullptr;
    }
}

pcie_irq_handler_retval_t PciInterruptDispatcher::IrqThunk(const PcieDevice& dev,
                                                           uint irq_id,
                                                           void* ctx) {
    DEBUG_ASSERT(ctx);

    Interrupt* interrupt = reinterpret_cast<Interrupt*>(ctx);
    if (!interrupt->timestamp) {
        // only record timestamp if this is the first IRQ since we started waiting
        interrupt->timestamp = current_time();
    }
    PciInterruptDispatcher* thiz
            = reinterpret_cast<PciInterruptDispatcher *>(interrupt->dispatcher);

    // Mask the IRQ at the PCIe hardware level if we can, and (if any threads
    // just became runable) tell the kernel to trigger a reschedule event.
    if (thiz->Signal(SIGNAL_MASK(interrupt->slot)) > 0) {
        return PCIE_IRQRET_MASK_AND_RESCHED;
    } else {
        return PCIE_IRQRET_MASK;
    }
}

zx_status_t PciInterruptDispatcher::Create(
        const fbl::RefPtr<PcieDevice>& device,
        uint32_t irq_id,
        bool maskable,
        zx_rights_t* out_rights,
        fbl::RefPtr<Dispatcher>* out_interrupt) {
    // Sanity check our args
    if (!device || !out_rights || !out_interrupt) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (!is_valid_interrupt(irq_id, 0)) {
        return ZX_ERR_INTERNAL;
    }

    fbl::AllocChecker ac;
    // Attempt to allocate a new dispatcher wrapper.
    auto interrupt_dispatcher = new (&ac) PciInterruptDispatcher(irq_id, maskable);
    fbl::RefPtr<Dispatcher> dispatcher = fbl::AdoptRef<Dispatcher>(interrupt_dispatcher);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    // Stash reference to the underlying device in the dispatcher we just
    // created, then attempt to register our dispatcher with the bus driver.
    DEBUG_ASSERT(device);
    interrupt_dispatcher->device_ = device;

    zx_status_t result = interrupt_dispatcher->AddSlot(IRQ_SLOT, irq_id, 0);
    if (result != ZX_OK) {
        interrupt_dispatcher->device_ = nullptr;
        return result;
    }

    // Everything seems to have gone well.  Make sure the interrupt is unmasked
    // (if it is maskable) then transfer our dispatcher refererence to the
    // caller.
    if (maskable) {
        device->UnmaskIrq(irq_id);
    }
    *out_interrupt = fbl::move(dispatcher);
    *out_rights    = ZX_DEFAULT_PCI_INTERRUPT_RIGHTS;
    return ZX_OK;
}

zx_status_t PciInterruptDispatcher::Bind(uint32_t slot, uint32_t vector, uint32_t options) {
    canary_.Assert();

    // PCI interrupt handles are automatically bound on creation and unbound on handle close
    return ZX_ERR_NOT_SUPPORTED;
}

zx_status_t PciInterruptDispatcher::WaitForInterrupt(uint64_t* out_slots) {
    canary_.Assert();

    return Wait(out_slots);
}

void PciInterruptDispatcher::on_zero_handles() {
    if (maskable_)
        device_->MaskIrq(irq_id_);

    Cancel();
}

void PciInterruptDispatcher::PreWait() {
    if (maskable_) {
        device_->UnmaskIrq(irq_id_);
    }

    for (auto& interrupt : interrupts_) {
        // clear timestamp so we can know when first IRQ occurs
        interrupt.timestamp = 0;
    }
}

void PciInterruptDispatcher::PostWait(uint64_t signals) {
    // nothing to do here
}

void PciInterruptDispatcher::MaskInterrupt(uint32_t vector) {
}

void PciInterruptDispatcher::UnmaskInterrupt(uint32_t vector) {
}

zx_status_t PciInterruptDispatcher::RegisterInterruptHandler(uint32_t vector, void* data) {
    return device_->RegisterIrqHandler(vector, IrqThunk, data);
}

void PciInterruptDispatcher::UnregisterInterruptHandler(uint32_t vector) {
    device_->RegisterIrqHandler(vector, nullptr, nullptr);
}

#endif  // if WITH_DEV_PCIE
