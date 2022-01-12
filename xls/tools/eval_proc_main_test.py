# Lint as: python3
#
# Copyright 2020 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import subprocess

from absl import logging

from xls.common import runfiles
from absl.testing import absltest

EVAL_PROC_MAIN_PATH = runfiles.get_path("xls/tools/eval_proc_main")

PROC_IR = """package foo

chan in_ch(bits[64], id=1, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata=\"\"\"\"\"\")
chan in_ch_2(bits[64], id=2, kind=streaming, ops=receive_only, flow_control=ready_valid, metadata=\"\"\"\"\"\")
chan out_ch(bits[64], id=3, kind=streaming, ops=send_only, flow_control=ready_valid, metadata=\"\"\"\"\"\")
chan out_ch_2(bits[64], id=4, kind=streaming, ops=send_only, flow_control=ready_valid, metadata=\"\"\"\"\"\")

proc test_proc(tkn: token, st: (bits[64]), init=(10)) {
  receive.1: (token, bits[64]) = receive(tkn, channel_id=1, id=1)

  literal.21: bits[64] = literal(value=10, id=21)
  tuple_index.23: bits[64] = tuple_index(st, index=0, id=23)

  literal.3: bits[1] = literal(value=1, id=3)
  tuple_index.7: token = tuple_index(receive.1, index=0, id=7)
  tuple_index.4: bits[64] = tuple_index(receive.1, index=1, id=4)
  receive.9: (token, bits[64]) = receive(tuple_index.7, channel_id=2, id=9)
  tuple_index.10: bits[64] = tuple_index(receive.9, index=1, id=10)
  add.8: bits[64] = add(tuple_index.4, tuple_index.10, id=8)
  add.24: bits[64] = add(add.8, tuple_index.23, id=24)

  tuple_index.11: token = tuple_index(receive.9, index=0, id=11)
  send.2: token = send(tuple_index.11, add.24, predicate=literal.3, channel_id=3, id=2)
  literal.14: bits[64] = literal(value=55, id=14)
  send.12: token = send(send.2, literal.14, predicate=literal.3, channel_id=4, id=12)

  add.20: bits[64] = add(literal.21, tuple_index.23, id=20)

  tuple.22: (bits[64]) = tuple(add.20, id=22)

  next(send.12, tuple.22)
}
"""


def run_command(args):
  """Runs the command described by args and returns the completion object."""
  # Don't use check=True because we want to print stderr/stdout on failure for a
  # better error message.
  # pylint: disable=subprocess-run-check
  comp = subprocess.run(
      args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8")
  if comp.returncode != 0:
    logging.error("Failed to run: %s", " ".join(args))
    logging.error("stderr: %s", comp.stderr)
    logging.error("stdout: %s", comp.stdout)
  comp.check_returncode()
  return comp


class EvalProcTest(absltest.TestCase):

  def test_basic(self):
    ir_file = self.create_tempfile(content=PROC_IR)
    input_file = self.create_tempfile(content="""
bits[64]:42
bits[64]:101
""")
    input_file_2 = self.create_tempfile(content="""
bits[64]:10
bits[64]:6
""")
    output_file = self.create_tempfile(content="""
bits[64]:62
bits[64]:127
""")
    output_file_2 = self.create_tempfile(content="""
bits[64]:55
bits[64]:55
""")

    shared_args = [
        EVAL_PROC_MAIN_PATH, ir_file.full_path, "--ticks", "2", "-v=3",
        "--logtostderr", "--inputs_for_channels",
        "in_ch={infile1},in_ch_2={infile2}".format(
            infile1=input_file.full_path,
            infile2=input_file_2.full_path), "--expected_outputs_for_channels",
        "out_ch={outfile},out_ch_2={outfile2}".format(
            outfile=output_file.full_path, outfile2=output_file_2.full_path)
    ]

    output = run_command(shared_args + ["--backend", "ir_interpreter"])
    self.assertIn("Proc test_proc", output.stderr)

    output = run_command(shared_args + ["--backend", "serial_jit"])
    self.assertIn("Proc test_proc", output.stderr)

  def test_reset_static(self):
    ir_file = self.create_tempfile(content=PROC_IR)
    input_file = self.create_tempfile(content="""
bits[64]:42
bits[64]:101
""")
    input_file_2 = self.create_tempfile(content="""
bits[64]:10
bits[64]:6
""")
    output_file = self.create_tempfile(content="""
bits[64]:62
bits[64]:117
""")
    output_file_2 = self.create_tempfile(content="""
bits[64]:55
bits[64]:55
""")

    shared_args = [
        EVAL_PROC_MAIN_PATH, ir_file.full_path, "--ticks", "1,1", "-v=3",
        "--logtostderr", "--inputs_for_channels",
        "in_ch={infile1},in_ch_2={infile2}".format(
            infile1=input_file.full_path,
            infile2=input_file_2.full_path), "--expected_outputs_for_channels",
        "out_ch={outfile},out_ch_2={outfile2}".format(
            outfile=output_file.full_path, outfile2=output_file_2.full_path)
    ]

    output = run_command(shared_args + ["--backend", "ir_interpreter"])
    self.assertIn("Proc test_proc", output.stderr)

    output = run_command(shared_args + ["--backend", "serial_jit"])
    self.assertIn("Proc test_proc", output.stderr)


if __name__ == "__main__":
  absltest.main()