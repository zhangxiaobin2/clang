#!/usr/bin/env python# This script is used to deal compilation database json file
# The major use is to translate "arguments" to "command" in the cdb json file
#
#project: https://github.com/rizsotto/Bear
# is a tool that capture compile command with gnu make(not cmake),
# and create the compilation database json file with field "arguments"(not "command").
# xtu-build.py and xtu-analyze.py does not support field "arguments" in the cdb json file,
# so I adapt the "arguments" to "command" for the original procedure.

def get_command_from_arguments(arguments):
  command_tmp = ""
  arguments.reverse()
  while len(arguments) > 0:
    opt = arguments.pop()
    command_tmp += opt + " "
    command_tmp.rstrip(" ")
  return command_tmp

def translate_arguments_to_command(one_tu):
  result = True
  if 'command' not in one_tu:
    if 'arguments' in one_tu:
      one_tu['command'] = get_command_from_arguments(one_tu['arguments'])
    else:
      print "No field \"command\" and \"arguments\" to deal, exit..."
      result = False
  return result
