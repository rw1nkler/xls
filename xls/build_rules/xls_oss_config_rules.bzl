# Copyright 2021 The XLS Authors
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

"""This module contains oss configurations for XLS build rules."""

CONFIG = {
    "xls_outs_attrs": {
        "outs": attr.string_list(
            doc = "The list of generated files.",
        ),
    },
}

def enable_generated_file_wrapper(**kwargs):  # @unused
    """The function is a placeholder for enable_generated_file_wrapper.

    The function is intended to be empty.

    Args:
      **kwargs: Keyword arguments. Named arguments.
    """
    pass
