/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <hypervisor.h>

#include "guest/instr_emul.h"

static void complete_ioreq(struct vhm_request *vhm_req)
{
	vhm_req->valid = 0;
	atomic_store32(&vhm_req->processed, REQ_STATE_FREE);
}

/**
 * @pre io_req->type == REQ_PORTIO
 *
 * @remark This function must be called when \p io_req is completed, after
 * either a previous call to emulate_io() returning 0 or the corresponding VHM
 * request having transferred to the COMPLETE state.
 */
static void
emulate_pio_post(struct vcpu *vcpu, struct io_request *io_req)
{
	struct pio_request *pio_req = &io_req->reqs.pio;
	uint64_t mask = 0xFFFFFFFFUL >> (32UL - 8UL * pio_req->size);

	if (pio_req->direction == REQUEST_READ) {
		uint64_t value = (uint64_t)pio_req->value;
		uint64_t rax = vcpu_get_gpreg(vcpu, CPU_REG_RAX);

		rax = ((rax) & ~mask) | (value & mask);
		vcpu_set_gpreg(vcpu, CPU_REG_RAX, rax);
	}
}

/**
 * @pre vcpu->req.type == REQ_PORTIO
 *
 * @remark This function must be called after the VHM request corresponding to
 * \p vcpu being transferred to the COMPLETE state.
 */
void dm_emulate_pio_post(struct vcpu *vcpu)
{
	uint16_t cur = vcpu->vcpu_id;
	union vhm_request_buffer *req_buf = NULL;
	struct io_request *io_req = &vcpu->req;
	struct pio_request *pio_req = &io_req->reqs.pio;
	struct vhm_request *vhm_req;

	req_buf = (union vhm_request_buffer *)(vcpu->vm->sw.io_shared_page);
	vhm_req = &req_buf->req_queue[cur];

	pio_req->value = vhm_req->reqs.pio.value;

	/* VHM emulation data already copy to req, mark to free slot now */
	complete_ioreq(vhm_req);

	emulate_pio_post(vcpu, io_req);
}

/**
 * @pre vcpu->req.type == REQ_MMIO
 *
 * @remark This function must be called when \p io_req is completed, after
 * either a previous call to emulate_io() returning 0 or the corresponding VHM
 * request having transferred to the COMPLETE state.
 */
void emulate_mmio_post(struct vcpu *vcpu, struct io_request *io_req)
{
	struct mmio_request *mmio_req = &io_req->reqs.mmio;

	if (mmio_req->direction == REQUEST_READ) {
		/* Emulate instruction and update vcpu register set */
		emulate_instruction(vcpu);
	}
}

/**
 * @pre vcpu->req.type == REQ_MMIO
 *
 * @remark This function must be called after the VHM request corresponding to
 * \p vcpu being transferred to the COMPLETE state.
 */
void dm_emulate_mmio_post(struct vcpu *vcpu)
{
	uint16_t cur = vcpu->vcpu_id;
	struct io_request *io_req = &vcpu->req;
	struct mmio_request *mmio_req = &io_req->reqs.mmio;
	union vhm_request_buffer *req_buf;
	struct vhm_request *vhm_req;

	req_buf = (union vhm_request_buffer *)(vcpu->vm->sw.io_shared_page);
	vhm_req = &req_buf->req_queue[cur];

	mmio_req->value = vhm_req->reqs.mmio.value;

	/* VHM emulation data already copy to req, mark to free slot now */
	complete_ioreq(vhm_req);

	emulate_mmio_post(vcpu, io_req);
}

#ifdef CONFIG_PARTITION_MODE
static void io_instr_dest_handler(struct io_request *io_req)
{
	struct pio_request *pio_req = &io_req->reqs.pio;

	if (pio_req->direction == REQUEST_READ) {
		pio_req->value = 0xFFFFFFFFU;
	}
	io_req->processed = REQ_STATE_COMPLETE;
}
#endif

void emulate_io_post(struct vcpu *vcpu)
{
	union vhm_request_buffer *req_buf;
	struct vhm_request *vhm_req;

	req_buf = (union vhm_request_buffer *)vcpu->vm->sw.io_shared_page;
	vhm_req = &req_buf->req_queue[vcpu->vcpu_id];

	if ((vhm_req->valid == 0) ||
		(atomic_load32(&vhm_req->processed) != REQ_STATE_COMPLETE)) {
		return;
	}

	/*
	 * If vcpu is in Zombie state and will be destroyed soon. Just
	 * mark ioreq done and don't resume vcpu.
	 */
	if (vcpu->state == VCPU_ZOMBIE) {
		complete_ioreq(vhm_req);
		return;
	}

	switch (vcpu->req.type) {
	case REQ_MMIO:
		request_vcpu_pre_work(vcpu, ACRN_VCPU_MMIO_COMPLETE);
		break;

	case REQ_PORTIO:
	case REQ_PCICFG:
		/* REQ_PORTIO on 0xcf8 & 0xcfc may switch to REQ_PCICFG in some
		 * cases. It works to apply the post-work for REQ_PORTIO on
		 * REQ_PCICFG because the format of the first 28 bytes of
		 * REQ_PORTIO & REQ_PCICFG requests are exactly the same and
		 * post-work is mainly interested in the read value.
		 */
		dm_emulate_pio_post(vcpu);
		break;

	default:
		/* REQ_WP can only be triggered on writes which do not need
		 * post-work. Just mark the ioreq done. */
		complete_ioreq(vhm_req);
		break;
	}

	resume_vcpu(vcpu);
}

/**
 * Try handling the given request by any port I/O handler registered in the
 * hypervisor.
 *
 * @pre io_req->type == REQ_PORTIO
 *
 * @return 0       - Successfully emulated by registered handlers.
 * @return -ENODEV - No proper handler found.
 * @return -EIO    - The request spans multiple devices and cannot be emulated.
 */
int32_t
hv_emulate_pio(struct vcpu *vcpu, struct io_request *io_req)
{
	int32_t status = -ENODEV;
	uint16_t port, size;
	uint32_t mask;
	struct vm *vm = vcpu->vm;
	struct pio_request *pio_req = &io_req->reqs.pio;
	struct vm_io_handler *handler;

	port = (uint16_t)pio_req->address;
	size = (uint16_t)pio_req->size;
	mask = 0xFFFFFFFFU >> (32U - 8U * size);

	for (handler = vm->arch_vm.io_handler;
		handler != NULL; handler = handler->next) {
		uint16_t base = handler->desc.addr;
		uint16_t end = base + (uint16_t)handler->desc.len;

		if ((port >= end) || (port + size <= base)) {
			continue;
		} else if (!((port >= base) && ((port + size) <= end))) {
			pr_fatal("Err:IO, port 0x%04x, size=%hu spans devices",
					port, size);
			status = -EIO;
			break;
		} else {
			if (pio_req->direction == REQUEST_WRITE) {
				handler->desc.io_write(handler, vm, port, size,
					pio_req->value & mask);

				pr_dbg("IO write on port %04x, data %08x", port,
					pio_req->value & mask);
			} else {
				pio_req->value = handler->desc.io_read(handler,
						vm, port, size);

				pr_dbg("IO read on port %04x, data %08x",
					port, pio_req->value);
			}
			status = 0;
			break;
		}
	}

	return status;
}

/**
 * Use registered MMIO handlers on the given request if it falls in the range of
 * any of them.
 *
 * @pre io_req->type == REQ_MMIO
 *
 * @return 0       - Successfully emulated by registered handlers.
 * @return -ENODEV - No proper handler found.
 * @return -EIO    - The request spans multiple devices and cannot be emulated.
 */
static int32_t
hv_emulate_mmio(struct vcpu *vcpu, struct io_request *io_req)
{
	int status = -ENODEV;
	uint64_t address, size;
	struct list_head *pos;
	struct mmio_request *mmio_req = &io_req->reqs.mmio;
	struct mem_io_node *mmio_handler = NULL;

	address = mmio_req->address;
	size = mmio_req->size;

	list_for_each(pos, &vcpu->vm->mmio_list) {
		uint64_t base, end;

		mmio_handler = list_entry(pos, struct mem_io_node, list);
		base = mmio_handler->range_start;
		end = mmio_handler->range_end;

		if ((address + size <= base) || (address >= end)) {
			continue;
		} else if (!((address >= base) && (address + size <= end))) {
			pr_fatal("Err MMIO, address:0x%llx, size:%x",
				 address, size);
			return -EIO;
		} else {
			/* Handle this MMIO operation */
			status = mmio_handler->read_write(vcpu, io_req,
					mmio_handler->handler_private_data);
			break;
		}
	}

	return status;
}

/**
 * Handle an I/O request by either invoking a hypervisor-internal handler or
 * deliver to VHM.
 *
 * @return 0       - Successfully emulated by registered handlers.
 * @return IOREQ_PENDING - The I/O request is delivered to VHM.
 * @return -EIO    - The request spans multiple devices and cannot be emulated.
 * @return Negative on other errors during emulation.
 */
int32_t
emulate_io(struct vcpu *vcpu, struct io_request *io_req)
{
	int32_t status;

	switch (io_req->type) {
	case REQ_PORTIO:
		status = hv_emulate_pio(vcpu, io_req);
		break;
	case REQ_MMIO:
	case REQ_WP:
		status = hv_emulate_mmio(vcpu, io_req);
		break;
	default:
		/* Unknown I/O request type */
		status = -EINVAL;
		break;
	}

	if (status == -ENODEV) {
#ifdef CONFIG_PARTITION_MODE
		/*
		 * No handler from HV side, return all FFs on read
		 * and discard writes.
		 */
		io_instr_dest_handler(io_req);
		status = 0;

#else
		/*
		 * No handler from HV side, search from VHM in Dom0
		 *
		 * ACRN insert request to VHM and inject upcall.
		 */
		status = acrn_insert_request_wait(vcpu, io_req);

		if (status != 0) {
			struct pio_request *pio_req = &io_req->reqs.pio;
			pr_fatal("Err:IO %s access to port 0x%04lx, size=%lu",
				(pio_req->direction != REQUEST_READ) ? "read" : "write",
				pio_req->address, pio_req->size);
		} else {
			status = IOREQ_PENDING;
		}
#endif
	}

	return status;
}

int32_t pio_instr_vmexit_handler(struct vcpu *vcpu)
{
	int32_t status;
	uint64_t exit_qual;
	int32_t cur_context_idx = vcpu->arch_vcpu.cur_context;
	struct io_request *io_req = &vcpu->req;
	struct pio_request *pio_req = &io_req->reqs.pio;

	exit_qual = vcpu->arch_vcpu.exit_qualification;

	io_req->type = REQ_PORTIO;
	pio_req->size = VM_EXIT_IO_INSTRUCTION_SIZE(exit_qual) + 1UL;
	pio_req->address = VM_EXIT_IO_INSTRUCTION_PORT_NUMBER(exit_qual);
	if (VM_EXIT_IO_INSTRUCTION_ACCESS_DIRECTION(exit_qual) == 0UL) {
		pio_req->direction = REQUEST_WRITE;
		pio_req->value = (uint32_t)vcpu_get_gpreg(vcpu, CPU_REG_RAX);
	} else {
		pio_req->direction = REQUEST_READ;
	}

	TRACE_4I(TRACE_VMEXIT_IO_INSTRUCTION,
		(uint32_t)pio_req->address,
		(uint32_t)pio_req->direction,
		(uint32_t)pio_req->size,
		(uint32_t)cur_context_idx);

	status = emulate_io(vcpu, io_req);

	if (status == 0) {
		emulate_pio_post(vcpu, io_req);
	} else if (status == IOREQ_PENDING) {
		status = 0;
	}

	return status;
}

static void register_io_handler(struct vm *vm, struct vm_io_handler *hdlr)
{
	if (vm->arch_vm.io_handler != NULL) {
		hdlr->next = vm->arch_vm.io_handler;
	}

	vm->arch_vm.io_handler = hdlr;
}

static void empty_io_handler_list(struct vm *vm)
{
	struct vm_io_handler *handler = vm->arch_vm.io_handler;
	struct vm_io_handler *tmp;

	while (handler != NULL) {
		tmp = handler;
		handler = tmp->next;
		free(tmp);
	}
	vm->arch_vm.io_handler = NULL;
}

void free_io_emulation_resource(struct vm *vm)
{
	empty_io_handler_list(vm);

	/* Free I/O emulation bitmaps */
	free(vm->arch_vm.iobitmap[0]);
	free(vm->arch_vm.iobitmap[1]);
}

void allow_guest_io_access(struct vm *vm, uint32_t address_arg, uint32_t nbytes)
{
	uint32_t address = address_arg;
	uint32_t *b;
	uint32_t i;
	uint32_t a;

	b = vm->arch_vm.iobitmap[0];
	for (i = 0U; i < nbytes; i++) {
		if ((address & 0x8000U) != 0U) {
			b = vm->arch_vm.iobitmap[1];
		}
		a = address & 0x7fffU;
		b[a >> 5U] &= ~(1U << (a & 0x1fU));
		address++;
	}
}

static void deny_guest_io_access(struct vm *vm, uint32_t address_arg, uint32_t nbytes)
{
	uint32_t address = address_arg;
	uint32_t *b;
	uint32_t i;
	uint32_t a;

	b = vm->arch_vm.iobitmap[0];
	for (i = 0U; i < nbytes; i++) {
		if ((address & 0x8000U) != 0U) {
			b = vm->arch_vm.iobitmap[1];
		}
		a = address & 0x7fffU;
		b[a >> 5U] |= (1U << (a & 0x1fU));
		address++;
	}
}

static struct vm_io_handler *create_io_handler(uint32_t port, uint32_t len,
				io_read_fn_t io_read_fn_ptr,
				io_write_fn_t io_write_fn_ptr)
{

	struct vm_io_handler *handler;

	handler = calloc(1U, sizeof(struct vm_io_handler));

	if (handler != NULL) {
		handler->desc.addr = port;
		handler->desc.len = len;
		handler->desc.io_read = io_read_fn_ptr;
		handler->desc.io_write = io_write_fn_ptr;
	} else {
		pr_err("Error: out of memory");
	}

	return handler;
}

void setup_io_bitmap(struct vm *vm)
{
	/* Allocate VM architecture state and IO bitmaps A and B */
	vm->arch_vm.iobitmap[0] = alloc_page();
	vm->arch_vm.iobitmap[1] = alloc_page();

	ASSERT((vm->arch_vm.iobitmap[0] != NULL) &&
	       (vm->arch_vm.iobitmap[1] != NULL), "");

	if (is_vm0(vm)) {
		(void)memset(vm->arch_vm.iobitmap[0], 0x00U, CPU_PAGE_SIZE);
		(void)memset(vm->arch_vm.iobitmap[1], 0x00U, CPU_PAGE_SIZE);
	} else {
		/* block all IO port access from Guest */
		(void)memset(vm->arch_vm.iobitmap[0], 0xFFU, CPU_PAGE_SIZE);
		(void)memset(vm->arch_vm.iobitmap[1], 0xFFU, CPU_PAGE_SIZE);
	}
}

void register_io_emulation_handler(struct vm *vm, struct vm_io_range *range,
		io_read_fn_t io_read_fn_ptr,
		io_write_fn_t io_write_fn_ptr)
{
	struct vm_io_handler *handler = NULL;

	if ((io_read_fn_ptr == NULL) || (io_write_fn_ptr == NULL)) {
		pr_err("Invalid IO handler.");
		return;
	}

	if (is_vm0(vm)) {
		deny_guest_io_access(vm, range->base, range->len);
	}

	handler = create_io_handler(range->base,
			range->len, io_read_fn_ptr, io_write_fn_ptr);

	register_io_handler(vm, handler);
}

int register_mmio_emulation_handler(struct vm *vm,
	hv_mem_io_handler_t read_write, uint64_t start,
	uint64_t end, void *handler_private_data)
{
	int status = -EINVAL;
	struct mem_io_node *mmio_node;

	if ((vm->hw.created_vcpus > 0U) && vm->hw.vcpu_array[0]->launched) {
		ASSERT(false, "register mmio handler after vm launched");
		return status;
	}

	/* Ensure both a read/write handler and range check function exist */
	if ((read_write != NULL) && (end > start)) {
		/* Allocate memory for node */
		mmio_node =
		(struct mem_io_node *)calloc(1U, sizeof(struct mem_io_node));

		/* Ensure memory successfully allocated */
		if (mmio_node != NULL) {
			/* Fill in information for this node */
			mmio_node->read_write = read_write;
			mmio_node->handler_private_data = handler_private_data;

			INIT_LIST_HEAD(&mmio_node->list);
			list_add(&mmio_node->list, &vm->mmio_list);

			mmio_node->range_start = start;
			mmio_node->range_end = end;

			/*
			 * SOS would map all its memory at beginning, so we
			 * should unmap it. But UOS will not, so we shouldn't
			 * need to unmap it.
			 */
			if (is_vm0(vm)) {
				ept_mr_del(vm,
					(uint64_t *)vm->arch_vm.nworld_eptp,
					start, end - start);
			}

			/* Return success */
			status = 0;
		}
	}

	/* Return status to caller */
	return status;
}

void unregister_mmio_emulation_handler(struct vm *vm, uint64_t start,
	uint64_t end)
{
	struct list_head *pos, *tmp;
	struct mem_io_node *mmio_node;

	list_for_each_safe(pos, tmp, &vm->mmio_list) {
		mmio_node = list_entry(pos, struct mem_io_node, list);

		if ((mmio_node->range_start == start) &&
			(mmio_node->range_end == end)) {
			/* assume only one entry found in mmio_list */
			list_del_init(&mmio_node->list);
			free(mmio_node);
			break;
		}
	}
}
