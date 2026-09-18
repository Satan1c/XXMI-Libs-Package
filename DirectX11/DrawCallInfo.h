#pragma once

#include <d3d11_1.h>
#include <vector>

struct TextureOverride;

// Per-draw memo of TextureOverride lookups: the result depends only on the
// resource and this draw's call info, and the same resource is commonly
// looked up several times per draw (pre and post lists, several sections or
// slots). Entries hold a reference so the resource address can't be reused
// mid-draw. Copies start empty; once full, further lookups aren't memoised.
struct TextureOverrideMemo
{
	static const unsigned capacity = 16;

	struct Entry {
		ID3D11Resource *resource;
		bool fuzzy_only;
		std::vector<TextureOverride*> matches;
	};
	Entry entries[capacity];
	unsigned count = 0;

	TextureOverrideMemo() {}
	TextureOverrideMemo(const TextureOverrideMemo&) {}
	TextureOverrideMemo& operator=(const TextureOverrideMemo&) { clear(); return *this; }
	~TextureOverrideMemo() { clear(); }

	void clear()
	{
		for (unsigned i = 0; i < count; i++) {
			entries[i].resource->Release();
			entries[i].matches.clear();
		}
		count = 0;
	}
};

// These values can now be exposed through draw_type and their values should
// not be changed. Any additions should be added to the end of the list.
enum class DrawCall {
	Invalid                      = 0,
	Draw                         = 1,
	DrawIndexed                  = 2,
	DrawInstanced                = 3,
	DrawIndexedInstanced         = 4,
	DrawInstancedIndirect        = 5,
	DrawIndexedInstancedIndirect = 6,
	DrawAuto                     = 7,
	Dispatch                     = 8,
	DispatchIndirect             = 9,
};

struct DrawCallInfo
{
	DrawCall type;

	UINT VertexCount, IndexCount, InstanceCount;
	UINT FirstVertex, FirstIndex, FirstInstance;
	UINT ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ;

	ID3D11Buffer **indirect_buffer;
	UINT args_offset;

	bool skip, hunting_skip;

	TextureOverrideMemo texture_override_memo;

	DrawCallInfo() :
		type(DrawCall::Invalid),
		VertexCount(0),
		IndexCount(0),
		InstanceCount(0),
		FirstVertex(0),
		FirstIndex(0),
		FirstInstance(0),
		ThreadGroupCountX(0),
		ThreadGroupCountY(0),
		ThreadGroupCountZ(0),
		indirect_buffer(NULL),
		args_offset(0),
		skip(false),
		hunting_skip(false)
	{}

	DrawCallInfo(DrawCall type,
			UINT VertexCount, UINT IndexCount, UINT InstanceCount,
			UINT FirstVertex, UINT FirstIndex, UINT FirstInstance,
			ID3D11Buffer **indirect_buffer, UINT args_offset,
			UINT ThreadGroupCountX = 0, UINT ThreadGroupCountY = 0, UINT ThreadGroupCountZ = 0) :
		type(type),
		VertexCount(VertexCount),
		IndexCount(IndexCount),
		InstanceCount(InstanceCount),
		FirstVertex(FirstVertex),
		FirstIndex(FirstIndex),
		FirstInstance(FirstInstance),
		ThreadGroupCountX(ThreadGroupCountX),
		ThreadGroupCountY(ThreadGroupCountY),
		ThreadGroupCountZ(ThreadGroupCountZ),
		indirect_buffer(indirect_buffer),
		args_offset(args_offset),
		skip(false),
		hunting_skip(false)
	{}
};

