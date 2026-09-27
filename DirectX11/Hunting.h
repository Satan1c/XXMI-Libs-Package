// Hunting
//  This code is to implement the Hunting mechanism as a separate compilation from the main wrapper code.
//  It implements all the shader management based on user input via key presses from Input.

#include "HackerDevice.h"

// Custom #include handler used to track which shaders need to be reloaded after an included file is modified
class MigotoIncludeHandler : public ID3DInclude
{
	std::vector<std::string> dir_stack;

	void push_dir(const char *path);
public:
	MigotoIncludeHandler(const char *path);

	STDMETHOD(Open)(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes);
	STDMETHOD(Close)(LPCVOID pData);
};

// Definitions handed to every HLSL compile, so that shader source does not have
// to hard code the register IniParams happens to be bound to. Owns the storage
// the D3D_SHADER_MACRO array points at, so it has to outlive the compile call -
// hence non-copyable, since a copy's array would point at the original.
class ShaderCompileMacros
{
	char ini_params_register[8];
	std::vector<D3D_SHADER_MACRO> macros;
public:
	// extra: additional definitions to keep, terminated by a NULL name.
	ShaderCompileMacros(const D3D_SHADER_MACRO *extra = NULL);
	ShaderCompileMacros(const ShaderCompileMacros&) = delete;
	ShaderCompileMacros& operator=(const ShaderCompileMacros&) = delete;

	const D3D_SHADER_MACRO* Get() const { return macros.data(); }
};

void TimeoutHuntingBuffers();
void ParseHuntingSection();
void DumpUsage(wchar_t *dir);

void RegisterVisitedIndexBufferNoLock(uint32_t hash);
void RegisterVisitedIndexBuffer(uint32_t hash);
void RegisterVisitedVertexBufferNoLock(uint32_t hash, uint32_t slot_id);
void RegisterVisitedVertexBuffer(uint32_t hash, uint32_t slot_id);
void PurgeStaleVisitedBufferHashes(HackerDevice* device);
