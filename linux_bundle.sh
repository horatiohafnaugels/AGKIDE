#! /bin/sh

cd /home/paul/Code/SVN/AGKTrunk
cp CompilerNew/build/AGKCompiler /home/paul/Programs/AGK/Tier1/Compiler/AGKCompiler
cp CompilerNew/CommandList.dat /home/paul/Programs/AGK/Tier1/Compiler/CommandList.dat
cp Broadcaster/AGKBroadcaster/build/AGKBroadcaster /home/paul/Programs/AGK/Tier1/Compiler/AGKBroadcaster
mkdir -p /home/paul/Programs/AGK/Tier1/Compiler/interpreters
cp apps/interpreter_linux/build/LinuxPlayer /home/paul/Programs/AGK/Tier1/Compiler/interpreters/LinuxPlayer
