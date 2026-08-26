#include "pch.h"
#include "Shared/McpServer.h"
#include "Shared/McpFramebuffer.h"
#include "Utilities/Base64.h"

std::string McpServer::ExecGetFramebuffer(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	McpFramebufferSnapshot snapshot;
	std::string error;
	if(!McpFramebuffer::CaptureNes(_emu, snapshot, error)) return ErrorResponse(cmd.id, error);
	std::string data = Base64::Encode(snapshot.Data);
	std::string result = "{\"frame\":" + std::to_string(snapshot.Frame)
		+ ",\"width\":" + std::to_string(snapshot.Width)
		+ ",\"height\":" + std::to_string(snapshot.Height)
		+ ",\"format\":\"nes16le\",\"encoding\":\"base64\",\"byteLength\":"
		+ std::to_string(snapshot.Data.size())
		+ ",\"hashAlgorithm\":\"md5\",\"hash\":\"" + snapshot.Hash
		+ "\",\"data\":\"" + data + "\"}";
	return OkResponse(cmd.id, result);
}
