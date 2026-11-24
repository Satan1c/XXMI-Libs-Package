#pragma once

#include <d3d11_1.h>
#include <stdint.h>

// Shader resource binds a command list run has made but not yet sent to D3D,
// so that consecutive ones can go out as few multi-slot calls.
// Only the bind is postponed, never the evaluation of what to bind:
// the sources, slots and views are all resolved when the command runs.
//
// The command list runtime flushes it before anything that could read or write a t slot,
// and at the end of every top level run, so nothing outside a run sees it.
class PendingBinds {
public:
	// Binds slots [first, first + count) of one stage with one call:
	typedef void (*Setter)(ID3D11DeviceContext1 *context, wchar_t shader_type, UINT first, UINT count,
			ID3D11ShaderResourceView *const *views);

	PendingBinds();
	~PendingBinds();
	PendingBinds(const PendingBinds&) = delete;
	PendingBinds& operator=(const PendingBinds&) = delete;

	// False, and nothing recorded, for a stage or slot that does not exist.
	// A null view is recorded like any other, and the last write to a slot wins:
	bool RecordShaderResource(wchar_t shader_type, unsigned slot, ID3D11ShaderResourceView *view);

	bool Empty() const { return !pending; }

	// Returns whether anything was bound:
	bool Flush(ID3D11DeviceContext1 *context, Setter setter);

private:
	static const unsigned STAGE_COUNT = 6;
	static const unsigned SLOT_COUNT = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
	static const unsigned MASK_WORDS = SLOT_COUNT / 64;

	struct Stage {
		// Never zeroed: an entry is valid only while its bit in dirty is set.
		ID3D11ShaderResourceView *views[SLOT_COUNT];
		uint64_t dirty[MASK_WORDS];
	};

	Stage stages[STAGE_COUNT];
	bool pending;

	bool FlushStage(Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context, Setter setter);
	static void ReleaseStage(Stage &stage);
};
