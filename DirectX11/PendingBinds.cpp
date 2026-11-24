#include "PendingBinds.h"

static_assert(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT % 64 == 0, "the dirty masks are whole words");
static_assert(D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT <= 64, "the constant buffer dirty mask is one word");

static const wchar_t STAGE_LETTERS[] = L"vhdgpc";

static int stage_index(wchar_t shader_type)
{
	for (int i = 0; STAGE_LETTERS[i]; i++) {
		if (STAGE_LETTERS[i] == shader_type)
			return i;
	}

	return -1;
}

// The first slot at or after `from` that is dirty, or clean when want_dirty is false.
// slot_count when there is none.
static unsigned next_slot(const uint64_t *dirty, unsigned slot_count, unsigned from, bool want_dirty)
{
	for (; from < slot_count; from++) {
		uint64_t word = want_dirty ? dirty[from / 64] : ~dirty[from / 64];
		uint64_t rest = word >> (from % 64);

		if (!rest) {
			from |= 63; // Nothing wanted in what is left of this word
			continue;
		}

		if (rest & 1)
			return from;
	}

	return slot_count;
}

// The buffer's description, or false for anything that is not a buffer a constant buffer slot takes.
// The type is asked before the description:
// the pointer may come from a cast of any resource.
static bool constant_buffer_desc(ID3D11Buffer *buffer, D3D11_BUFFER_DESC *desc)
{
	D3D11_RESOURCE_DIMENSION dimension;
	buffer->GetType(&dimension);
	if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
		return false;

	buffer->GetDesc(desc);

	return desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER;
}

// A window is given in constants, and both ends must be multiples of 16 of them.
static bool valid_window(UINT first, UINT count)
{
	return !(first % 16) && !(count % 16) && count && count <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT;
}

static bool can_share_call(ID3D11Buffer *buffer, bool region, UINT first, UINT count)
{
	if (region && !valid_window(first, count))
		return false;

	if (!buffer)
		return true;

	D3D11_BUFFER_DESC desc;
	if (!constant_buffer_desc(buffer, &desc))
		return false;

	return !region || (uint64_t)first + count <= desc.ByteWidth / 16;
}

PendingBinds::PendingBinds() :
	pending(false),
	held(0),
	run_depth(0)
{
	for (Stage &stage : stages) {
		for (uint64_t &word : stage.dirty)
			word = 0;
		stage.cb_dirty = 0;
	}
}

PendingBinds::~PendingBinds()
{
	for (Stage &stage : stages)
		ReleaseStage(stage);
}

bool PendingBinds::RecordShaderResource(wchar_t shader_type, unsigned slot, ID3D11ShaderResourceView *view)
{
	int index = stage_index(shader_type);
	if (index < 0 || slot >= SLOT_COUNT)
		return false;

	Stage &stage = stages[index];
	uint64_t &word = stage.dirty[slot / 64];
	uint64_t bit = 1ull << (slot % 64);

	// The new reference first: the view being replaced may be this very one.
	if (view)
		view->AddRef();

	if ((word & bit) && stage.views[slot])
		stage.views[slot]->Release();

	stage.views[slot] = view;
	word |= bit;
	pending = true;

	return true;
}

bool PendingBinds::RecordConstantBuffer(wchar_t shader_type, unsigned slot, ID3D11Buffer *buffer, bool region, UINT first, UINT count)
{
	int index = stage_index(shader_type);
	if (index < 0 || slot >= CB_SLOT_COUNT)
		return false;

	if (!can_share_call(buffer, region, first, count))
		return false;

	Stage &stage = stages[index];
	uint64_t bit = 1ull << slot;

	// The new reference first: the buffer being replaced may be this very one.
	if (buffer)
		buffer->AddRef();

	if ((stage.cb_dirty & bit) && stage.cbs[slot].buffer)
		stage.cbs[slot].buffer->Release();

	stage.cbs[slot] = { buffer, first, count, region };
	stage.cb_dirty |= bit;
	pending = true;

	return true;
}

bool PendingBinds::Flush(ID3D11DeviceContext1 *context, Setter setter, ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter)
{
	bool bound = false;

	for (unsigned i = 0; i < STAGE_COUNT; i++) {
		if (FlushStage(stages[i], STAGE_LETTERS[i], context, setter))
			bound = true;
		if (FlushConstantBuffers(stages[i], STAGE_LETTERS[i], context, cb_setter, cb_region_setter))
			bound = true;
		ReleaseStage(stages[i]);
	}

	pending = false;

	return bound;
}

// One call per contiguous run of dirty slots.
// Gaps are never filled in: that would need the slot's current binding,
// and a slot nobody recorded (IniParams at t120 by default) must stay as it is.
bool PendingBinds::FlushStage(Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context, Setter setter)
{
	bool bound = false;
	unsigned first = next_slot(stage.dirty, SLOT_COUNT, 0, true);

	while (first < SLOT_COUNT) {
		unsigned end = next_slot(stage.dirty, SLOT_COUNT, first, false);

		setter(context, shader_type, first, end - first, &stage.views[first]);
		bound = true;

		first = next_slot(stage.dirty, SLOT_COUNT, end, true);
	}

	return bound;
}

// One call per contiguous run of dirty slots that bind alike.
bool PendingBinds::FlushConstantBuffers(Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context,
		ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter)
{
	bool bound = false;
	unsigned first = next_slot(&stage.cb_dirty, CB_SLOT_COUNT, 0, true);

	while (first < CB_SLOT_COUNT) {
		unsigned end = ConstantBufferRunEnd(stage, first);

		BindConstantBufferRun(stage, shader_type, context, first, end, cb_setter, cb_region_setter);
		bound = true;

		first = next_slot(&stage.cb_dirty, CB_SLOT_COUNT, end, true);
	}

	return bound;
}

// Slots bound with and without a window are different calls, so a run ends where that changes.
unsigned PendingBinds::ConstantBufferRunEnd(const Stage &stage, unsigned first)
{
	unsigned end = first + 1;

	while (end < CB_SLOT_COUNT && (stage.cb_dirty >> end & 1) && stage.cbs[end].region == stage.cbs[first].region)
		end++;

	return end;
}

void PendingBinds::BindConstantBufferRun(const Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context, unsigned first, unsigned end,
		ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter)
{
	ID3D11Buffer *buffers[CB_SLOT_COUNT];
	UINT cb_first[CB_SLOT_COUNT];
	UINT cb_counts[CB_SLOT_COUNT];

	for (unsigned slot = first; slot < end; slot++) {
		buffers[slot - first] = stage.cbs[slot].buffer;
		cb_first[slot - first] = stage.cbs[slot].first;
		cb_counts[slot - first] = stage.cbs[slot].count;
	}

	if (stage.cbs[first].region)
		cb_region_setter(context, shader_type, first, end - first, buffers, cb_first, cb_counts);
	else
		cb_setter(context, shader_type, first, end - first, buffers);
}

// The masks are cleared before the references go,
// so a release that ends up back here finds nothing left to release.
void PendingBinds::ReleaseStage(Stage &stage)
{
	uint64_t released[MASK_WORDS];
	uint64_t cb_released = stage.cb_dirty;

	for (unsigned i = 0; i < MASK_WORDS; i++) {
		released[i] = stage.dirty[i];
		stage.dirty[i] = 0;
	}
	stage.cb_dirty = 0;

	for (unsigned slot = 0; slot < SLOT_COUNT; slot++) {
		if ((released[slot / 64] >> (slot % 64) & 1) && stage.views[slot])
			stage.views[slot]->Release();
	}

	for (unsigned slot = 0; slot < CB_SLOT_COUNT; slot++) {
		if ((cb_released >> slot & 1) && stage.cbs[slot].buffer)
			stage.cbs[slot].buffer->Release();
	}
}
