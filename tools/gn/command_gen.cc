// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/atomicops.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/elapsed_timer.h"
#include "tools/gn/build_settings.h"
#include "tools/gn/commands.h"
#include "tools/gn/ninja_target_writer.h"
#include "tools/gn/ninja_writer.h"
#include "tools/gn/scheduler.h"
#include "tools/gn/setup.h"
#include "tools/gn/standard_out.h"

namespace commands {

namespace {

// Suppress output on success.
const char kSwitchQuiet[] = "q";

void BackgroundDoWrite(const Target* target, const Toolchain* toolchain) {
  NinjaTargetWriter::RunAndWriteFile(target, toolchain);
  g_scheduler->DecrementWorkCount();
}

// Called on the main thread.
void ItemResolvedCallback(base::subtle::Atomic32* write_counter,
                          scoped_refptr<Builder> builder,
                          const Item* item) {
  base::subtle::NoBarrier_AtomicIncrement(write_counter, 1);

  const Target* target = item->AsTarget();
  if (target) {
    const Toolchain* toolchain =
        builder->GetToolchain(target->settings()->toolchain_label());
    DCHECK(toolchain);
    g_scheduler->IncrementWorkCount();
    g_scheduler->ScheduleWork(
        base::Bind(&BackgroundDoWrite, target, toolchain));
  }
}

}  // namespace

const char kGen[] = "gen";
const char kGen_HelpShort[] =
    "gen: Generate ninja files.";
const char kGen_Help[] =
    "gn gen: Generate ninja files.\n"
    "\n"
    "  gn gen <output_directory>\n"
    "\n"
    "  Generates ninja files from the current tree and puts them in the given\n"
    "  output directory.\n"
    "\n"
    "  The output directory can be a source-repo-absolute path name such as:\n"
    "      //out/foo\n"
    "  Or it can be a directory relative to the current directory such as:\n"
    "      out/foo\n"
    "\n"
    "  See \"gn help\" for the common command-line switches.\n";

// Note: partially duplicated in command_gyp.cc.
int RunGen(const std::vector<std::string>& args) {
  base::ElapsedTimer timer;

  if (args.size() != 1) {
    Err(Location(), "Need exactly one build directory to generate.",
        "I expected something more like \"gn gen out/foo\"\n"
        "You can also see \"gn help gen\".").PrintToStdout();
    return 1;
  }

  // Deliberately leaked to avoid expensive process teardown.
  Setup* setup = new Setup();
  if (!setup->DoSetup(args[0]))
    return 1;

  // Cause the load to also generate the ninja files for each target. We wrap
  // the writing to maintain a counter.
  base::subtle::Atomic32 write_counter = 0;
  setup->builder()->set_resolved_callback(
      base::Bind(&ItemResolvedCallback, &write_counter,
                 scoped_refptr<Builder>(setup->builder())));

  // Do the actual load. This will also write out the target ninja files.
  if (!setup->Run())
    return 1;

  // Write the root ninja files.
  if (!NinjaWriter::RunAndWriteFiles(&setup->build_settings(),
                                     setup->builder()))
    return 1;

  base::TimeDelta elapsed_time = timer.Elapsed();

  if (!CommandLine::ForCurrentProcess()->HasSwitch(kSwitchQuiet)) {
    OutputString("Done. ", DECORATION_GREEN);

    std::string stats = "Wrote " +
        base::IntToString(static_cast<int>(write_counter)) +
        " targets from " +
        base::IntToString(
            setup->scheduler().input_file_manager()->GetInputFileCount()) +
        " files in " +
        base::IntToString(elapsed_time.InMilliseconds()) + "ms\n";
    OutputString(stats);
  }

  return 0;
}

}  // namespace commands
