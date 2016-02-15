// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/gn/ninja_binary_target_writer.h"

#include <sstream>
#include <utility>

#include "testing/gtest/include/gtest/gtest.h"
#include "tools/gn/scheduler.h"
#include "tools/gn/target.h"
#include "tools/gn/test_with_scope.h"

TEST(NinjaBinaryTargetWriter, SourceSet) {
  TestWithScope setup;
  Err err;

  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  Target target(setup.settings(), Label(SourceDir("//foo/"), "bar"));
  target.set_output_type(Target::SOURCE_SET);
  target.visibility().SetPublic();
  target.sources().push_back(SourceFile("//foo/input1.cc"));
  target.sources().push_back(SourceFile("//foo/input2.cc"));
  // Also test object files, which should be just passed through to the
  // dependents to link.
  target.sources().push_back(SourceFile("//foo/input3.o"));
  target.sources().push_back(SourceFile("//foo/input4.obj"));
  // Also test custom asm file extensions
  target.sources().push_back(SourceFile("//foo/input5.asm"));
  target.sources().push_back(SourceFile("//foo/input6.s"));
  target.sources().push_back(SourceFile("//foo/input7.arm"));
  // Also test unspecified asm file extension, which should be ignored.
  target.sources().push_back(SourceFile("//foo/input8.S"));
  target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(target.OnResolved(&err));

  // Source set itself.
  {
    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&target, out);
    writer.Run();

    const char expected[] =
        "defines =\n"
        "include_dirs =\n"
        "asmflags =\n"
        "cflags =\n"
        "cppflags =\n"
        "cflags_cc =\n"
        "cppflags_cc =\n"
        "root_out_dir = .\n"
        "target_out_dir = obj/foo\n"
        "target_output_name = bar\n"
        "\n"
        "build obj/foo/bar.input1.o: cxx ../../foo/input1.cc\n"
        "  source_name_part = input1\n"
        "  source_out_dir = obj/foo\n"
        "build obj/foo/bar.input2.o: cxx ../../foo/input2.cc\n"
        "  source_name_part = input2\n"
        "  source_out_dir = obj/foo\n"
        "build obj/foo/bar.input5.o: asm ../../foo/input5.asm\n"
        "  source_name_part = input5\n"
        "  source_out_dir = obj/foo\n"
        "build obj/foo/bar.input6.o: asm ../../foo/input6.s\n"
        "  source_name_part = input6\n"
        "  source_out_dir = obj/foo\n"
        "build obj/foo/bar.input7.o: asm ../../foo/input7.arm\n"
        "  source_name_part = input7\n"
        "  source_out_dir = obj/foo\n"
        "\n"
        "build obj/foo/bar.stamp: stamp obj/foo/bar.input1.o "
            "obj/foo/bar.input2.o ../../foo/input3.o ../../foo/input4.obj "
            "obj/foo/bar.input5.o obj/foo/bar.input6.o obj/foo/bar.input7.o\n";
    std::string out_str = out.str();
    EXPECT_EQ(expected, out_str);
  }

  // A shared library that depends on the source set.
  Target shlib_target(setup.settings(), Label(SourceDir("//foo/"), "shlib"));
  shlib_target.set_output_type(Target::SHARED_LIBRARY);
  shlib_target.public_deps().push_back(LabelTargetPair(&target));
  shlib_target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(shlib_target.OnResolved(&err));

  {
    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&shlib_target, out);
    writer.Run();

    const char expected[] =
        "defines =\n"
        "include_dirs =\n"
        "root_out_dir = .\n"
        "target_out_dir = obj/foo\n"
        "target_output_name = libshlib\n"
        "\n"
        "\n"
        // Ordering of the obj files here should come out in the order
        // specified, with the target's first, followed by the source set's, in
        // order.
        "build ./libshlib.so: solink obj/foo/bar.input1.o "
            "obj/foo/bar.input2.o ../../foo/input3.o ../../foo/input4.obj "
            "obj/foo/bar.input5.o obj/foo/bar.input6.o obj/foo/bar.input7.o "
            "|| obj/foo/bar.stamp\n"
        "  ldflags =\n"
        "  libs =\n"
        "  output_extension = .so\n";
    std::string out_str = out.str();
    EXPECT_EQ(expected, out_str);
  }

  // A static library that depends on the source set (should not link it).
  Target stlib_target(setup.settings(), Label(SourceDir("//foo/"), "stlib"));
  stlib_target.set_output_type(Target::STATIC_LIBRARY);
  stlib_target.public_deps().push_back(LabelTargetPair(&target));
  stlib_target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(stlib_target.OnResolved(&err));

  {
    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&stlib_target, out);
    writer.Run();

    const char expected[] =
        "defines =\n"
        "include_dirs =\n"
        "root_out_dir = .\n"
        "target_out_dir = obj/foo\n"
        "target_output_name = libstlib\n"
        "\n"
        "\n"
        // There are no sources so there are no params to alink. (In practice
        // this will probably fail in the archive tool.)
        "build obj/foo/libstlib.a: alink || obj/foo/bar.stamp\n"
        "  output_extension = \n";
    std::string out_str = out.str();
    EXPECT_EQ(expected, out_str);
  }

  // Make the static library 'complete', which means it should be linked.
  stlib_target.set_complete_static_lib(true);
  {
    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&stlib_target, out);
    writer.Run();

    const char expected[] =
        "defines =\n"
        "include_dirs =\n"
        "root_out_dir = .\n"
        "target_out_dir = obj/foo\n"
        "target_output_name = libstlib\n"
        "\n"
        "\n"
        // Ordering of the obj files here should come out in the order
        // specified, with the target's first, followed by the source set's, in
        // order.
        "build obj/foo/libstlib.a: alink obj/foo/bar.input1.o "
            "obj/foo/bar.input2.o ../../foo/input3.o ../../foo/input4.obj "
            "obj/foo/bar.input5.o obj/foo/bar.input6.o obj/foo/bar.input7.o "
            "|| obj/foo/bar.stamp\n"
        "  output_extension = \n";
    std::string out_str = out.str();
    EXPECT_EQ(expected, out_str);
  }
}

// This tests that output extension overrides apply, and input dependencies
// are applied.
TEST(NinjaBinaryTargetWriter, ProductExtensionAndInputDeps) {
  TestWithScope setup;
  Err err;

  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  // An action for our library to depend on.
  Target action(setup.settings(), Label(SourceDir("//foo/"), "action"));
  action.set_output_type(Target::ACTION_FOREACH);
  action.visibility().SetPublic();
  action.SetToolchain(setup.toolchain());
  ASSERT_TRUE(action.OnResolved(&err));

  // A shared library w/ the product_extension set to a custom value.
  Target target(setup.settings(), Label(SourceDir("//foo/"), "shlib"));
  target.set_output_type(Target::SHARED_LIBRARY);
  target.set_output_extension(std::string("so.6"));
  target.sources().push_back(SourceFile("//foo/input1.cc"));
  target.sources().push_back(SourceFile("//foo/input2.cc"));
  target.public_deps().push_back(LabelTargetPair(&action));
  target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(target.OnResolved(&err));

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&target, out);
  writer.Run();

  const char expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = libshlib\n"
      "\n"
      "build obj/foo/libshlib.input1.o: cxx ../../foo/input1.cc"
        " || obj/foo/action.stamp\n"
        "  source_name_part = input1\n"
        "  source_out_dir = obj/foo\n"
      "build obj/foo/libshlib.input2.o: cxx ../../foo/input2.cc"
        " || obj/foo/action.stamp\n"
        "  source_name_part = input2\n"
        "  source_out_dir = obj/foo\n"
      "\n"
      "build ./libshlib.so.6: solink obj/foo/libshlib.input1.o "
      // The order-only dependency here is stricly unnecessary since the
      // sources list this as an order-only dep. See discussion in the code
      // that writes this.
          "obj/foo/libshlib.input2.o || obj/foo/action.stamp\n"
      "  ldflags =\n"
      "  libs =\n"
      "  output_extension = .so.6\n";

  std::string out_str = out.str();
  EXPECT_EQ(expected, out_str);
}

// Tests libs are applied.
TEST(NinjaBinaryTargetWriter, LibsAndLibDirs) {
  TestWithScope setup;
  Err err;

  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  // A shared library w/ libs and lib_dirs.
  Target target(setup.settings(), Label(SourceDir("//foo/"), "shlib"));
  target.set_output_type(Target::SHARED_LIBRARY);
  target.config_values().libs().push_back(LibFile(SourceFile("//foo/lib1.a")));
  target.config_values().libs().push_back(LibFile("foo"));
  target.config_values().lib_dirs().push_back(SourceDir("//foo/bar/"));
  target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(target.OnResolved(&err));

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&target, out);
  writer.Run();

  const char expected[] =
      "defines =\n"
      "include_dirs =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = libshlib\n"
      "\n"
      "\n"
      "build ./libshlib.so: solink | ../../foo/lib1.a\n"
      "  ldflags = -L../../foo/bar\n"
      "  libs = ../../foo/lib1.a -lfoo\n"
      "  output_extension = .so\n";

  std::string out_str = out.str();
  EXPECT_EQ(expected, out_str);
}

TEST(NinjaBinaryTargetWriter, EmptyProductExtension) {
  TestWithScope setup;
  Err err;

  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  // This test is the same as ProductExtension, except that
  // we call set_output_extension("") and ensure that we still get the default.
  Target target(setup.settings(), Label(SourceDir("//foo/"), "shlib"));
  target.set_output_type(Target::SHARED_LIBRARY);
  target.set_output_extension(std::string());
  target.sources().push_back(SourceFile("//foo/input1.cc"));
  target.sources().push_back(SourceFile("//foo/input2.cc"));

  target.SetToolchain(setup.toolchain());
  ASSERT_TRUE(target.OnResolved(&err));

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&target, out);
  writer.Run();

  const char expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = libshlib\n"
      "\n"
      "build obj/foo/libshlib.input1.o: cxx ../../foo/input1.cc\n"
        "  source_name_part = input1\n"
        "  source_out_dir = obj/foo\n"
      "build obj/foo/libshlib.input2.o: cxx ../../foo/input2.cc\n"
        "  source_name_part = input2\n"
        "  source_out_dir = obj/foo\n"
      "\n"
      "build ./libshlib.so: solink obj/foo/libshlib.input1.o "
          "obj/foo/libshlib.input2.o\n"
      "  ldflags =\n"
      "  libs =\n"
      "  output_extension = .so\n";

  std::string out_str = out.str();
  EXPECT_EQ(expected, out_str);
}

TEST(NinjaBinaryTargetWriter, SourceSetDataDeps) {
  TestWithScope setup;
  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  Err err;

  // This target is a data (runtime) dependency of the intermediate target.
  Target data(setup.settings(), Label(SourceDir("//foo/"), "data_target"));
  data.set_output_type(Target::EXECUTABLE);
  data.visibility().SetPublic();
  data.SetToolchain(setup.toolchain());
  ASSERT_TRUE(data.OnResolved(&err));

  // Intermediate source set target.
  Target inter(setup.settings(), Label(SourceDir("//foo/"), "inter"));
  inter.set_output_type(Target::SOURCE_SET);
  inter.visibility().SetPublic();
  inter.data_deps().push_back(LabelTargetPair(&data));
  inter.SetToolchain(setup.toolchain());
  inter.sources().push_back(SourceFile("//foo/inter.cc"));
  ASSERT_TRUE(inter.OnResolved(&err)) << err.message();

  // Write out the intermediate target.
  std::ostringstream inter_out;
  NinjaBinaryTargetWriter inter_writer(&inter, inter_out);
  inter_writer.Run();

  // The intermediate source set will be a stamp file that depends on the
  // object files, and will have an order-only dependency on its data dep and
  // data file.
  const char inter_expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = inter\n"
      "\n"
      "build obj/foo/inter.inter.o: cxx ../../foo/inter.cc\n"
        "  source_name_part = inter\n"
        "  source_out_dir = obj/foo\n"
      "\n"
      "build obj/foo/inter.stamp: stamp obj/foo/inter.inter.o || "
          "./data_target\n";
  EXPECT_EQ(inter_expected, inter_out.str());

  // Final target.
  Target exe(setup.settings(), Label(SourceDir("//foo/"), "exe"));
  exe.set_output_type(Target::EXECUTABLE);
  exe.public_deps().push_back(LabelTargetPair(&inter));
  exe.SetToolchain(setup.toolchain());
  exe.sources().push_back(SourceFile("//foo/final.cc"));
  ASSERT_TRUE(exe.OnResolved(&err));

  std::ostringstream final_out;
  NinjaBinaryTargetWriter final_writer(&exe, final_out);
  final_writer.Run();

  // The final output depends on both object files (one from the final target,
  // one from the source set) and has an order-only dependency on the source
  // set's stamp file and the final target's data file. The source set stamp
  // dependency will create an implicit order-only dependency on the data
  // target.
  const char final_expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = exe\n"
      "\n"
      "build obj/foo/exe.final.o: cxx ../../foo/final.cc\n"
        "  source_name_part = final\n"
        "  source_out_dir = obj/foo\n"
      "\n"
      "build ./exe: link obj/foo/exe.final.o obj/foo/inter.inter.o || "
          "obj/foo/inter.stamp\n"
      "  ldflags =\n"
      "  libs =\n"
      "  output_extension = \n";
  EXPECT_EQ(final_expected, final_out.str());
}

TEST(NinjaBinaryTargetWriter, SharedLibraryModuleDefinitionFile) {
  TestWithScope setup;
  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  Target shared_lib(setup.settings(), Label(SourceDir("//foo/"), "bar"));
  shared_lib.set_output_type(Target::SHARED_LIBRARY);
  shared_lib.SetToolchain(setup.toolchain());
  shared_lib.sources().push_back(SourceFile("//foo/sources.cc"));
  shared_lib.sources().push_back(SourceFile("//foo/bar.def"));

  Err err;
  ASSERT_TRUE(shared_lib.OnResolved(&err));

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&shared_lib, out);
  writer.Run();

  const char expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = libbar\n"
      "\n"
      "build obj/foo/libbar.sources.o: cxx ../../foo/sources.cc\n"
      "  source_name_part = sources\n"
      "  source_out_dir = obj/foo\n"
      "\n"
      "build ./libbar.so: solink obj/foo/libbar.sources.o | ../../foo/bar.def\n"
      "  ldflags = /DEF:../../foo/bar.def\n"
      "  libs =\n"
      "  output_extension = .so\n";
  EXPECT_EQ(expected, out.str());
}

TEST(NinjaBinaryTargetWriter, LoadableModule) {
  TestWithScope setup;
  setup.build_settings()->SetBuildDir(SourceDir("//out/Debug/"));

  Target loadable_module(setup.settings(), Label(SourceDir("//foo/"), "bar"));
  loadable_module.set_output_type(Target::LOADABLE_MODULE);
  loadable_module.visibility().SetPublic();
  loadable_module.SetToolchain(setup.toolchain());
  loadable_module.sources().push_back(SourceFile("//foo/sources.cc"));

  Err err;
  ASSERT_TRUE(loadable_module.OnResolved(&err)) << err.message();

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&loadable_module, out);
  writer.Run();

  const char loadable_expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = libbar\n"
      "\n"
      "build obj/foo/libbar.sources.o: cxx ../../foo/sources.cc\n"
      "  source_name_part = sources\n"
      "  source_out_dir = obj/foo\n"
      "\n"
      "build ./libbar.so: solink_module obj/foo/libbar.sources.o\n"
      "  ldflags =\n"
      "  libs =\n"
      "  output_extension = .so\n";
  EXPECT_EQ(loadable_expected, out.str());

  // Final target.
  Target exe(setup.settings(), Label(SourceDir("//foo/"), "exe"));
  exe.set_output_type(Target::EXECUTABLE);
  exe.public_deps().push_back(LabelTargetPair(&loadable_module));
  exe.SetToolchain(setup.toolchain());
  exe.sources().push_back(SourceFile("//foo/final.cc"));
  ASSERT_TRUE(exe.OnResolved(&err)) << err.message();

  std::ostringstream final_out;
  NinjaBinaryTargetWriter final_writer(&exe, final_out);
  final_writer.Run();

  // The final output depends on the loadable module so should have an
  // order-only dependency on the loadable modules's output file.
  const char final_expected[] =
      "defines =\n"
      "include_dirs =\n"
      "cflags =\n"
      "cppflags =\n"
      "cflags_cc =\n"
      "cppflags_cc =\n"
      "root_out_dir = .\n"
      "target_out_dir = obj/foo\n"
      "target_output_name = exe\n"
      "\n"
      "build obj/foo/exe.final.o: cxx ../../foo/final.cc\n"
      "  source_name_part = final\n"
      "  source_out_dir = obj/foo\n"
      "\n"
      "build ./exe: link obj/foo/exe.final.o || ./libbar.so\n"
      "  ldflags =\n"
      "  libs =\n"
      "  output_extension = \n";
  EXPECT_EQ(final_expected, final_out.str());
}

TEST(NinjaBinaryTargetWriter, WinPrecompiledHeaders) {
  Err err;

  // This setup's toolchain does not have precompiled headers defined.
  TestWithScope setup;

  // A precompiled header toolchain.
  Settings pch_settings(setup.build_settings(), "withpch/");
  Toolchain pch_toolchain(&pch_settings,
                          Label(SourceDir("//toolchain/"), "withpch"));
  pch_settings.set_toolchain_label(pch_toolchain.label());
  pch_settings.set_default_toolchain_label(setup.toolchain()->label());

  // Declare a C++ compiler that supports PCH.
  scoped_ptr<Tool> cxx_tool(new Tool);
  TestWithScope::SetCommandForTool(
      "c++ {{source}} {{cflags}} {{cflags_cc}} {{defines}} {{include_dirs}} "
      "-o {{output}}",
      cxx_tool.get());
  cxx_tool->set_outputs(SubstitutionList::MakeForTest(
      "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o"));
  cxx_tool->set_precompiled_header_type(Tool::PCH_MSVC);
  pch_toolchain.SetTool(Toolchain::TYPE_CXX, std::move(cxx_tool));

  // Add a C compiler as well.
  scoped_ptr<Tool> cc_tool(new Tool);
  TestWithScope::SetCommandForTool(
      "cc {{source}} {{cflags}} {{cflags_c}} {{defines}} {{include_dirs}} "
      "-o {{output}}",
      cc_tool.get());
  cc_tool->set_outputs(SubstitutionList::MakeForTest(
      "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o"));
  cc_tool->set_precompiled_header_type(Tool::PCH_MSVC);
  pch_toolchain.SetTool(Toolchain::TYPE_CC, std::move(cc_tool));
  pch_toolchain.ToolchainSetupComplete();

  // This target doesn't specify precompiled headers.
  {
    Target no_pch_target(&pch_settings,
                         Label(SourceDir("//foo/"), "no_pch_target"));
    no_pch_target.set_output_type(Target::SOURCE_SET);
    no_pch_target.visibility().SetPublic();
    no_pch_target.sources().push_back(SourceFile("//foo/input1.cc"));
    no_pch_target.sources().push_back(SourceFile("//foo/input2.c"));
    no_pch_target.config_values().cflags_c().push_back("-std=c99");
    no_pch_target.SetToolchain(&pch_toolchain);
    ASSERT_TRUE(no_pch_target.OnResolved(&err));

    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&no_pch_target, out);
    writer.Run();

    const char no_pch_expected[] =
        "defines =\n"
        "include_dirs =\n"
        "cflags =\n"
        "cflags_c = -std=c99\n"
        "cflags_cc =\n"
        "target_output_name = no_pch_target\n"
        "\n"
        "build withpch/obj/foo/no_pch_target.input1.o: "
               "withpch_cxx ../../foo/input1.cc\n"
        "  source_name_part = input1\n"
        "  source_out_dir = withpch/obj/foo\n"
        "build withpch/obj/foo/no_pch_target.input2.o: "
               "withpch_cc ../../foo/input2.c\n"
        "  source_name_part = input2\n"
        "  source_out_dir = withpch/obj/foo\n"
        "\n"
        "build withpch/obj/foo/no_pch_target.stamp: "
               "withpch_stamp withpch/obj/foo/no_pch_target.input1.o "
               "withpch/obj/foo/no_pch_target.input2.o\n";
    EXPECT_EQ(no_pch_expected, out.str());
  }

  // This target specifies PCH.
  {
    Target pch_target(&pch_settings,
                      Label(SourceDir("//foo/"), "pch_target"));
    pch_target.config_values().set_precompiled_header("build/precompile.h");
    pch_target.config_values().set_precompiled_source(
        SourceFile("//build/precompile.cc"));
    pch_target.set_output_type(Target::SOURCE_SET);
    pch_target.visibility().SetPublic();
    pch_target.sources().push_back(SourceFile("//foo/input1.cc"));
    pch_target.sources().push_back(SourceFile("//foo/input2.c"));
    pch_target.SetToolchain(&pch_toolchain);
    ASSERT_TRUE(pch_target.OnResolved(&err));

    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&pch_target, out);
    writer.Run();

    const char pch_win_expected[] =
        "defines =\n"
        "include_dirs =\n"
        "cflags =\n"
        // It should output language-specific pch files.
        "cflags_c = /Fpwithpch/obj/foo/pch_target_c.pch "
                    "/Yubuild/precompile.h\n"
        "cflags_cc = /Fpwithpch/obj/foo/pch_target_cc.pch "
                     "/Yubuild/precompile.h\n"
        "target_output_name = pch_target\n"
        "\n"
        // Compile the precompiled source files with /Yc.
        "build withpch/obj/build/pch_target.precompile.c.o: "
               "withpch_cc ../../build/precompile.cc\n"
        "  source_name_part = precompile\n"
        "  source_out_dir = withpch/obj/build\n"
        "  cflags_c = ${cflags_c} /Ycbuild/precompile.h\n"
        "\n"
        "build withpch/obj/build/pch_target.precompile.cc.o: "
               "withpch_cxx ../../build/precompile.cc\n"
        "  source_name_part = precompile\n"
        "  source_out_dir = withpch/obj/build\n"
        "  cflags_cc = ${cflags_cc} /Ycbuild/precompile.h\n"
        "\n"
        "build withpch/obj/foo/pch_target.input1.o: "
               "withpch_cxx ../../foo/input1.cc | "
               // Explicit dependency on the PCH build step.
               "withpch/obj/build/pch_target.precompile.cc.o\n"
        "  source_name_part = input1\n"
        "  source_out_dir = withpch/obj/foo\n"
        "build withpch/obj/foo/pch_target.input2.o: "
               "withpch_cc ../../foo/input2.c | "
               // Explicit dependency on the PCH build step.
               "withpch/obj/build/pch_target.precompile.c.o\n"
        "  source_name_part = input2\n"
        "  source_out_dir = withpch/obj/foo\n"
        "\n"
        "build withpch/obj/foo/pch_target.stamp: withpch_stamp "
               "withpch/obj/foo/pch_target.input1.o "
               "withpch/obj/foo/pch_target.input2.o "
               // The precompiled object files were added to the outputs.
               "withpch/obj/build/pch_target.precompile.c.o "
               "withpch/obj/build/pch_target.precompile.cc.o\n";
    EXPECT_EQ(pch_win_expected, out.str());
  }
}

TEST(NinjaBinaryTargetWriter, GCCPrecompiledHeaders) {
  Err err;

  // This setup's toolchain does not have precompiled headers defined.
  TestWithScope setup;

  // A precompiled header toolchain.
  Settings pch_settings(setup.build_settings(), "withpch/");
  Toolchain pch_toolchain(&pch_settings,
                          Label(SourceDir("//toolchain/"), "withpch"));
  pch_settings.set_toolchain_label(pch_toolchain.label());
  pch_settings.set_default_toolchain_label(setup.toolchain()->label());

  // Declare a C++ compiler that supports PCH.
  scoped_ptr<Tool> cxx_tool(new Tool);
  TestWithScope::SetCommandForTool(
      "c++ {{source}} {{cflags}} {{cflags_cc}} {{defines}} {{include_dirs}} "
      "-o {{output}}",
      cxx_tool.get());
  cxx_tool->set_outputs(SubstitutionList::MakeForTest(
      "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o"));
  cxx_tool->set_precompiled_header_type(Tool::PCH_GCC);
  pch_toolchain.SetTool(Toolchain::TYPE_CXX, std::move(cxx_tool));
  pch_toolchain.ToolchainSetupComplete();

  // Add a C compiler as well.
  scoped_ptr<Tool> cc_tool(new Tool);
  TestWithScope::SetCommandForTool(
      "cc {{source}} {{cflags}} {{cflags_c}} {{defines}} {{include_dirs}} "
      "-o {{output}}",
      cc_tool.get());
  cc_tool->set_outputs(SubstitutionList::MakeForTest(
      "{{source_out_dir}}/{{target_output_name}}.{{source_name_part}}.o"));
  cc_tool->set_precompiled_header_type(Tool::PCH_GCC);
  pch_toolchain.SetTool(Toolchain::TYPE_CC, std::move(cc_tool));
  pch_toolchain.ToolchainSetupComplete();

  // This target doesn't specify precompiled headers.
  {
    Target no_pch_target(&pch_settings,
                         Label(SourceDir("//foo/"), "no_pch_target"));
    no_pch_target.set_output_type(Target::SOURCE_SET);
    no_pch_target.visibility().SetPublic();
    no_pch_target.sources().push_back(SourceFile("//foo/input1.cc"));
    no_pch_target.sources().push_back(SourceFile("//foo/input2.c"));
    no_pch_target.config_values().cflags_c().push_back("-std=c99");
    no_pch_target.SetToolchain(&pch_toolchain);
    ASSERT_TRUE(no_pch_target.OnResolved(&err));

    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&no_pch_target, out);
    writer.Run();

    const char no_pch_expected[] =
        "defines =\n"
        "include_dirs =\n"
        "cflags =\n"
        "cflags_c = -std=c99\n"
        "cflags_cc =\n"
        "target_output_name = no_pch_target\n"
        "\n"
        "build withpch/obj/foo/no_pch_target.input1.o: "
               "withpch_cxx ../../foo/input1.cc\n"
        "  source_name_part = input1\n"
        "  source_out_dir = withpch/obj/foo\n"
        "build withpch/obj/foo/no_pch_target.input2.o: "
               "withpch_cc ../../foo/input2.c\n"
        "  source_name_part = input2\n"
        "  source_out_dir = withpch/obj/foo\n"
        "\n"
        "build withpch/obj/foo/no_pch_target.stamp: "
               "withpch_stamp withpch/obj/foo/no_pch_target.input1.o "
               "withpch/obj/foo/no_pch_target.input2.o\n";
    EXPECT_EQ(no_pch_expected, out.str());
  }

  // This target specifies PCH.
  {
    Target pch_target(&pch_settings,
                      Label(SourceDir("//foo/"), "pch_target"));
    pch_target.config_values().set_precompiled_header("build/precompile.h");
    pch_target.config_values().set_precompiled_source(
        SourceFile("//build/precompile.h"));
    pch_target.config_values().cflags_c().push_back("-std=c99");
    pch_target.set_output_type(Target::SOURCE_SET);
    pch_target.visibility().SetPublic();
    pch_target.sources().push_back(SourceFile("//foo/input1.cc"));
    pch_target.sources().push_back(SourceFile("//foo/input2.c"));
    pch_target.SetToolchain(&pch_toolchain);
    ASSERT_TRUE(pch_target.OnResolved(&err));

    std::ostringstream out;
    NinjaBinaryTargetWriter writer(&pch_target, out);
    writer.Run();

    const char pch_gcc_expected[] =
        "defines =\n"
        "include_dirs =\n"
        "cflags =\n"
        "cflags_c = -std=c99 "
                    "-include withpch/obj/build/pch_target.precompile.h-c\n"
        "cflags_cc = -include withpch/obj/build/pch_target.precompile.h-cc\n"
        "target_output_name = pch_target\n"
        "\n"
        // Compile the precompiled sources with -x <lang>.
        "build withpch/obj/build/pch_target.precompile.h-c.gch: "
               "withpch_cc ../../build/precompile.h\n"
        "  source_name_part = precompile\n"
        "  source_out_dir = withpch/obj/build\n"
        "  cflags_c = -std=c99 -x c-header\n"
        "\n"
        "build withpch/obj/build/pch_target.precompile.h-cc.gch: "
               "withpch_cxx ../../build/precompile.h\n"
        "  source_name_part = precompile\n"
        "  source_out_dir = withpch/obj/build\n"
        "  cflags_cc = -x c++-header\n"
        "\n"
        "build withpch/obj/foo/pch_target.input1.o: "
               "withpch_cxx ../../foo/input1.cc | "
               // Explicit dependency on the PCH build step.
               "withpch/obj/build/pch_target.precompile.h-cc.gch\n"
        "  source_name_part = input1\n"
        "  source_out_dir = withpch/obj/foo\n"
        "build withpch/obj/foo/pch_target.input2.o: "
               "withpch_cc ../../foo/input2.c | "
               // Explicit dependency on the PCH build step.
               "withpch/obj/build/pch_target.precompile.h-c.gch\n"
        "  source_name_part = input2\n"
        "  source_out_dir = withpch/obj/foo\n"
        "\n"
        "build withpch/obj/foo/pch_target.stamp: "
               "withpch_stamp withpch/obj/foo/pch_target.input1.o "
               "withpch/obj/foo/pch_target.input2.o\n";
    EXPECT_EQ(pch_gcc_expected, out.str());
  }
}

// Should throw an error with the scheduler if a duplicate object file exists.
// This is dependent on the toolchain's object file mapping.
TEST(NinjaBinaryTargetWriter, DupeObjFileError) {
  Scheduler scheduler;

  TestWithScope setup;
  TestTarget target(setup, "//foo:bar", Target::EXECUTABLE);
  target.sources().push_back(SourceFile("//a.cc"));
  target.sources().push_back(SourceFile("//a.cc"));

  EXPECT_FALSE(scheduler.is_failed());

  std::ostringstream out;
  NinjaBinaryTargetWriter writer(&target, out);
  writer.Run();

  // Should have issued an error.
  EXPECT_TRUE(scheduler.is_failed());
}
