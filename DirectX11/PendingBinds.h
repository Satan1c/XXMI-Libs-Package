#pragma once

#include <d3d11_1.h>
#include <stdint.h>

// Shader resource and constant buffer binds a command list run has made but not yet sent to D3D,
// so that consecutive ones can go out as few multi-slot calls.
// Only the bind is postponed, never the evaluation of what to bind:
// the sources, slots and views are all resolved when the command runs.
//
// The command list runtime flushes it before anything that could read or write a t or cb slot,
// and at the end of every top level run, so nothing outside a run sees it.
class PendingBinds {
public:
	// Binds slots [first, first + count) of one stage with one call:
	typedef void (*Setter)(ID3D11DeviceContext1 *context, wchar_t shader_type, UINT first, UINT count,
			ID3D11ShaderResourceView *const *views);
	typedef void (*ConstantBufferSetter)(ID3D11DeviceContext1 *context, wchar_t shader_type, UINT first, UINT count,
			ID3D11Buffer *const *buffers);
	// Windows are given in constants of 16 bytes, as XXSetConstantBuffers1 takes them:
	typedef void (*ConstantBufferRegionSetter)(ID3D11DeviceContext1 *context, wchar_t shader_type, UINT first, UINT count,
			ID3D11Buffer *const *buffers, const UINT *cb_first, const UINT *cb_counts);

	PendingBinds();
	~PendingBinds();
	PendingBinds(const PendingBinds&) = delete;
	PendingBinds& operator=(const PendingBinds&) = delete;

	// False, and nothing recorded, for a stage or slot that does not exist.
	// A null view is recorded like any other, and the last write to a slot wins:
	bool RecordShaderResource(wchar_t shader_type, unsigned slot, ID3D11ShaderResourceView *view);

	// False, and nothing recorded, when the bind cannot share a multi-slot call with others:
	// the runtime drops the whole call if any one buffer or window in it is invalid,
	// so what it would reject, or what this cannot tell is valid, is left to be bound on its own.
	// A bind with a window goes out by a different call from one without, so a run of slots is split between them.
	bool RecordConstantBuffer(wchar_t shader_type, unsigned slot, ID3D11Buffer *buffer, bool region, UINT first, UINT count);

	bool Empty() const { return !pending; }

	// Returns whether anything was bound:
	bool Flush(ID3D11DeviceContext1 *context, Setter setter, ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter);

private:
	static const unsigned STAGE_COUNT = 6;
	static const unsigned SLOT_COUNT = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
	static const unsigned MASK_WORDS = SLOT_COUNT / 64;
	static const unsigned CB_SLOT_COUNT = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;

	struct ConstantBuffer {
		ID3D11Buffer *buffer;
		UINT first;
		UINT count;
		bool region;
	};

	struct Stage {
		// Never zeroed: an entry is valid only while its bit in dirty is set.
		ID3D11ShaderResourceView *views[SLOT_COUNT];
		uint64_t dirty[MASK_WORDS];
		// Likewise, with the bits in cb_dirty:
		ConstantBuffer cbs[CB_SLOT_COUNT];
		uint64_t cb_dirty;
	};

	Stage stages[STAGE_COUNT];
	bool pending;

	bool FlushStage(Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context, Setter setter);
	bool FlushConstantBuffers(Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context,
			ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter);
	static unsigned ConstantBufferRunEnd(const Stage &stage, unsigned first);
	static void BindConstantBufferRun(const Stage &stage, wchar_t shader_type, ID3D11DeviceContext1 *context, unsigned first, unsigned end,
			ConstantBufferSetter cb_setter, ConstantBufferRegionSetter cb_region_setter);
	static void ReleaseStage(Stage &stage);
};
