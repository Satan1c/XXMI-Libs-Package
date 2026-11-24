#include "PendingBinds.h"

static_assert(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT % 64 == 0, "the dirty masks are whole words");

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

PendingBinds::PendingBinds() :
	pending(false)
{
	for (Stage &stage : stages) {
		for (uint64_t &word : stage.dirty)
			word = 0;
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

bool PendingBinds::Flush(ID3D11DeviceContext1 *context, Setter setter)
{
	bool bound = false;

	for (unsigned i = 0; i < STAGE_COUNT; i++) {
		if (FlushStage(stages[i], STAGE_LETTERS[i], context, setter))
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

// The masks are cleared before the references go,
// so a release that ends up back here finds nothing left to release.
void PendingBinds::ReleaseStage(Stage &stage)
{
	uint64_t released[MASK_WORDS];

	for (unsigned i = 0; i < MASK_WORDS; i++) {
		released[i] = stage.dirty[i];
		stage.dirty[i] = 0;
	}

	for (unsigned slot = 0; slot < SLOT_COUNT; slot++) {
		if ((released[slot / 64] >> (slot % 64) & 1) && stage.views[slot])
			stage.views[slot]->Release();
	}
}
