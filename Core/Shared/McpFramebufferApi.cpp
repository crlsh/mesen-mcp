#include "pch.h"
#include "Shared/McpServer.h"
#include "Shared/McpFramebuffer.h"
#include "Shared/McpFramebufferRecorder.h"
#include "Shared/Emulator.h"
#include "Shared/Interfaces/IConsole.h"
#include "Utilities/Base64.h"

namespace {
	std::string FramebufferCaptureStatusJson(const McpFramebufferRecorderStatus& status)
	{
		std::string result = std::string("{\"enabled\":") + (status.Enabled ? "true" : "false")
			+ ",\"frames\":" + std::to_string(status.Frames)
			+ ",\"lastFrame\":" + (status.HasLastFrame ? std::to_string(status.LastFrame) : "null")
			+ ",\"writeFailures\":" + std::to_string(status.WriteFailures)
			+ ",\"width\":256,\"height\":240,\"format\":\"nes16le\",\"hashAlgorithm\":\"md5\"}";
		return result;
	}
}

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

std::string McpServer::ExecStartFramebufferCapture(McpTypedCommand& cmd)
{
	if(_coreState.phase != EmuPhase::Running) return ErrorResponse(cmd.id, "emulator not running");
	IConsole* console = _emu->GetConsoleUnsafe();
	if(!console || console->GetConsoleType() != ConsoleType::Nes) return ErrorResponse(cmd.id, "NES console required");
	std::string error;
	if(!McpFramebufferRecorder::Instance().Start(cmd.path, error)) return ErrorResponse(cmd.id, error);
	return OkResponse(cmd.id, R"({"started":true,"width":256,"height":240,"format":"nes16le","hashAlgorithm":"md5","rowSchema":{"frame":"uint32","fbHash":"lowercase hex"}})");
}

std::string McpServer::ExecStopFramebufferCapture(McpTypedCommand& cmd)
{
	McpFramebufferRecorderStatus status = McpFramebufferRecorder::Instance().Stop();
	return OkResponse(cmd.id, FramebufferCaptureStatusJson(status));
}

std::string McpServer::ExecGetFramebufferCaptureStatus(McpTypedCommand& cmd)
{
	return OkResponse(cmd.id, FramebufferCaptureStatusJson(McpFramebufferRecorder::Instance().GetStatus()));
}
