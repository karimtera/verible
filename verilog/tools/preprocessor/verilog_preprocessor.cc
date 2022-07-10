// Copyright 2017-2020 The Verible Authors.
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

#include <functional>
#include <iostream>
#include <string>

#include "absl/flags/usage.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/util/file_util.h"
#include "common/util/init_command_line.h"
#include "common/util/subcommand.h"
#include "verilog/transform/strip_comments.h"
#include "verilog/parser/verilog_lexer.h"
#include "verilog/preprocessor/verilog_preprocess.h"

using verible::SubcommandArgsRange;
using verible::SubcommandEntry;

static absl::Status StripComments(const SubcommandArgsRange& args,
                                  std::istream&, std::ostream& outs,
                                  std::ostream&) {
  if (args.empty()) {
    return absl::InvalidArgumentError(
        "Missing file argument.  Use '-' for stdin.");
  }
  const char* source_file = args[0];
  std::string source_contents;
  if (auto status = verible::file::GetContents(source_file, &source_contents);
      !status.ok()) {
    return status;
  }

  // TODO(fangism): parse a subcommand-specific flag for replacement option
  // See documentation for 'replacement' arg of the library function:
  verilog::StripVerilogComments(source_contents, &outs, ' ');

  return absl::OkStatus();
}

static absl::Status MultipleCU(const SubcommandArgsRange& args,
                                  std::istream&, std::ostream& outs,
                                  std::ostream&) {
  if (args.empty()) {
    return absl::InvalidArgumentError(
        "Missing file argument.");
  }

  for(auto source_file:args){
    std::string source_contents;
    if (auto status = verible::file::GetContents(source_file, &source_contents);
        !status.ok()) {
      return status;
    }
    verilog::VerilogPreprocess::Config config;
    config.filter_branches=1;
    verilog::VerilogPreprocess preprocessor(config);
    verilog::VerilogLexer lexer(source_contents);
    verible::TokenSequence lexed_sequence;
      for (lexer.DoNextToken(); !lexer.GetLastToken().isEOF();
        lexer.DoNextToken()) {
        // For now we will store the syntax tree tokens only, ignoring all the white-space characters.
        // however that should be stored to output the source code just like it was, but with conditionals filtered.
        if(lexer.KeepSyntaxTreeTokens(lexer.GetLastToken())) lexed_sequence.push_back(lexer.GetLastToken());
    }
    verible::TokenStreamView lexed_streamview;
    // Initializing the lexed token stream view.
    InitTokenStreamView(lexed_sequence, &lexed_streamview);

    verilog::VerilogPreprocessData preprocessed_data = preprocessor.ScanStream(lexed_streamview);
    auto& preprocessed_stream = preprocessed_data.preprocessed_token_stream;
    for(auto u:preprocessed_stream) outs<<*u<<'\n'; // output the preprocessed tokens.
    for(auto u:preprocessed_data.errors) outs<<u.error_message<<'\n'; // for debugging.

  }
  return absl::OkStatus();
}

static const std::pair<absl::string_view, SubcommandEntry> kCommands[] = {
    {"strip-comments",  //
     {&StripComments,   //
      R"(strip-comments file

Input:
  'file' is a Verilog or SystemVerilog source file.
  Use '-' to read from stdin.

Output: (stdout)
  Contents of original file with // and /**/ comments removed.
)"}},

    {"multiple-cu",  //
     {&MultipleCU,   //
      R"(multiple-cu file1 file2 ...

Input:
  'file's are Verilog or SystemVerilog source files.
  each one will be preprocessed in a separate compilation unit.
Output: (stdout)
  Contents of original file with compiler directives interpreted.
)"}},
};

int main(int argc, char* argv[]) {
  // Create a registry of subcommands (locally, rather than as a static global).
  verible::SubcommandRegistry commands;
  for (const auto& entry : kCommands) {
    const auto status = commands.RegisterCommand(entry.first, entry.second);
    if (!status.ok()) {
      std::cerr << status.message() << std::endl;
      return 2;  // fatal error
    }
  }

  const std::string usage = absl::StrCat("usage: ", argv[0],
                                         " command args...\n"
                                         "available commands:\n",
                                         commands.ListCommands());

  // Process invocation args.
  const auto args = verible::InitCommandLine(usage, &argc, &argv);
  if (args.size() == 1) {
    std::cerr << absl::ProgramUsageMessage() << std::endl;
    return 1;
  }
  // args[0] is the program name
  // args[1] is the subcommand
  // subcommand args start at [2]
  const SubcommandArgsRange command_args(args.cbegin() + 2, args.cend());

  const auto& sub = commands.GetSubcommandEntry(args[1]);
  // Run the subcommand.
  const auto status = sub.main(command_args, std::cin, std::cout, std::cerr);
  if (!status.ok()) {
    std::cerr << status.message() << std::endl;
    return 1;
  }
  return 0;
}
