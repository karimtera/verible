// Copyright 2021 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <iomanip>  // Only needed for json debugging right now
#include <iostream>

#include "common/lsp/json-rpc-dispatcher.h"
#include "common/lsp/lsp-protocol.h"
#include "common/lsp/message-stream-splitter.h"
#include "common/util/init_command_line.h"
#include "verilog/tools/ls/lsp-parse-buffer.h"
#include "verilog/tools/ls/verible-lsp-adapter.h"

// Windows specific implementation of read()
#ifndef _WIN32
#include <unistd.h>
#else
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
// Windows doesn't have Posix read(), but something called _read
#define read(fd, buf, size) _read(fd, buf, size)
#endif

using nlohmann::json;
using verible::lsp::BufferCollection;
using verible::lsp::InitializeResult;
using verible::lsp::JsonRpcDispatcher;
using verible::lsp::MessageStreamSplitter;

static std::string GetVersionNumber() {
  return "0.0 alpha";  // TODO(hzeller): once ready, extract from build version
}

// The "initialize" method requests server capabilities.
static InitializeResult InitializeServer(const nlohmann::json &params) {
  // Ignore passed client capabilities from params right now,
  // just announce what we do.
  InitializeResult result;
  result.serverInfo = {
      .name = "Verible Verilog language server.",
      .version = GetVersionNumber(),
  };
  result.capabilities = {
      {
          "textDocumentSync",
          {
              {"openClose", true},  // Want open/close events
              {"change", 2},        // Incremental updates
          },
      },
      {"codeActionProvider", true},               // Autofixes for lint errors
      {"documentSymbolProvider", true},           // Symbol-outline of file
      {"documentRangeFormattingProvider", true},  // Format selection
      {"documentFormattingProvider", true},       // Full file format
      {"documentHighlightProvider", true},        // Highlight same symbol
      {"diagnosticProvider",                      // Pull model of diagnostics.
       {
           {"interFileDependencies", false},
           {"workspaceDiagnostics", false},
       }},
  };

  return result;
}

// Publish a diagnostic sent to the server.
static void SendDiagnostics(const std::string &uri,
                            const verilog::BufferTracker &buffer_tracker,
                            JsonRpcDispatcher *dispatcher) {
  // TODO(hzeller): Cache result and rate-limit.
  // This should not send anything if the diagnostics we're about to
  // send would be exactly the same as last time.
  verible::lsp::PublishDiagnosticsParams params;

  // For the diagnostic notification (that we send somewhat unsolicited), we
  // limit the number of diagnostic messages. In the
  // textDocument/diagnostic RPC request, we send all of them.
  // Arbitrary limit here. Maybe set with flag ?
  static constexpr int kDiagnosticLimit = 500;
  params.uri = uri;
  params.diagnostics =
      verilog::CreateDiagnostics(buffer_tracker, kDiagnosticLimit);
  dispatcher->SendNotification("textDocument/publishDiagnostics", params);
}

int main(int argc, char *argv[]) {
  verible::InitCommandLine(argv[0], &argc, &argv);

#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  std::cerr << "Verible Alpha Language Server " << GetVersionNumber()
            << std::endl;

  // Input and output is stdin and stdout
  static constexpr int in_fd = 0;  // STDIN_FILENO
  JsonRpcDispatcher::WriteFun write_fun = [](absl::string_view reply) {
    // Output formatting as header/body chunk as required by LSP spec.
    std::cout << "Content-Length: " << reply.size() << "\r\n\r\n";
    std::cout << reply << std::flush;
  };

  // Stream splitter splits the input stream into messages (header/body).
  MessageStreamSplitter stream_splitter;
  JsonRpcDispatcher dispatcher(write_fun);

  // All bodies the stream splitter extracts are pushed to the json dispatcher
  stream_splitter.SetMessageProcessor(
      [&dispatcher](absl::string_view /*header*/, absl::string_view body) {
        return dispatcher.DispatchMessage(body);
      });

  // The buffer collection keeps track of all the buffers opened in the editor.
  // It registers callbacks to receive the relevant events on the dispatcher.
  BufferCollection buffers(&dispatcher);

  // The parsed buffers convert the raw text into the SystemVerilog parsed
  // representation, basis for all high-level features we provide from it.
  // Subscribe them to text edit updates to trigger parse.
  verilog::BufferTrackerContainer parsed_buffers;
  buffers.SetChangeListener(parsed_buffers.GetSubscriptionCallback());

  // Whenever there is a new parse result ready, use that as an opportunity
  // to send diagnostics to the client.
  parsed_buffers.SetChangeListener(
      [&dispatcher](const std::string &uri,
                    const verilog::BufferTracker &buffer_tracker) {
        SendDiagnostics(uri, buffer_tracker, &dispatcher);
      });

  // -- Register JSON RPC callbacks

  // Exchange of capabilities.
  dispatcher.AddRequestHandler("initialize", InitializeServer);

  dispatcher.AddRequestHandler(  // Provide diagnostics on request
      "textDocument/diagnostic",
      [&parsed_buffers](const verible::lsp::DocumentDiagnosticParams &p) {
        return verilog::GenerateDiagnosticReport(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });

  dispatcher.AddRequestHandler(  // Provide autofixes
      "textDocument/codeAction",
      [&parsed_buffers](const verible::lsp::CodeActionParams &p) {
        return verilog::GenerateLinterCodeActions(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });

  dispatcher.AddRequestHandler(  // Provide document outline/index
      "textDocument/documentSymbol",
      [&parsed_buffers](const verible::lsp::DocumentSymbolParams &p) {
        return verilog::CreateDocumentSymbolOutline(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });

  dispatcher.AddRequestHandler(  // Highlight related symbols under cursor
      "textDocument/documentHighlight",
      [&parsed_buffers](const verible::lsp::DocumentHighlightParams &p) {
        return verilog::CreateHighlightRanges(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });

  dispatcher.AddRequestHandler(  // format range of file
      "textDocument/rangeFormatting",
      [&parsed_buffers](const verible::lsp::DocumentFormattingParams &p) {
        return verilog::FormatRange(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });
  dispatcher.AddRequestHandler(  // format entire file
      "textDocument/formatting",
      [&parsed_buffers](const verible::lsp::DocumentFormattingParams &p) {
        return verilog::FormatRange(
            parsed_buffers.FindBufferTrackerOrNull(p.textDocument.uri), p);
      });

  // The client sends a request to shut down. Use that to exit our loop.
  bool shutdown_requested = false;
  dispatcher.AddRequestHandler("shutdown",
                               [&shutdown_requested](const nlohmann::json &) {
                                 shutdown_requested = true;
                                 return nullptr;
                               });

  absl::Status status = absl::OkStatus();
  while (status.ok() && !shutdown_requested) {
    status = stream_splitter.PullFrom([](char *buf, int size) -> int {  //
      return read(in_fd, buf, size);
    });
  }

  std::cerr << status.message() << std::endl;

  if (shutdown_requested) {
    std::cerr << "Shutting down due to shutdown request." << std::endl;
  }

  std::cerr << "Statistics" << std::endl;
  std::cerr << "Largest message seen: "
            << stream_splitter.StatLargestBodySeen() / 1024 << " kiB "
            << std::endl;
  for (const auto &stats : dispatcher.GetStatCounters()) {
    fprintf(stderr, "%30s %9d\n", stats.first.c_str(), stats.second);
  }
}
