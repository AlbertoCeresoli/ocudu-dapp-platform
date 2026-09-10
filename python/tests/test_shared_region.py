# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
"""Adversarial descriptor/layout tests; no radio or /dev/shm writes required."""
import dataclasses
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ocudu_e3.shared_region import (REGION_HEADER, REGION_MAGIC, ROW_HEADER,
    PosixSharedLocation, SharedRegion, SharedRegionError, SharedRegionReader, TensorDescriptor)


class SharedRegionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "region"
        self.region = SharedRegion("posixShared", "/region", 320, 64, 128, 2, 1)
        self.data = bytearray(320)
        REGION_HEADER.pack_into(self.data, 0, REGION_MAGIC, 1, 64, 64, 2, 128, 0, 2)
        ROW_HEADER.pack_into(self.data, 64, 2, 1, 4, 1, 0, 0, 0, 1)
        self.data[128:132] = b"data"
        self.path.write_bytes(self.data)
        self.desc = TensorDescriptor("posixShared", PosixSharedLocation("/region", 0, 128), 4, 2)

    def reader(self, region=None, **kwargs):
        return SharedRegionReader(region or self.region, self.tmp.name, **kwargs)

    def test_valid_copy_and_generation_recheck(self):
        with self.reader() as reader:
            self.assertEqual(reader.read(self.desc), b"data")
            with patch.object(reader, "_row_generation", side_effect=[2, 4]):
                self.assertIsNone(reader.read(self.desc))
            self.assertIsNone(reader.read(dataclasses.replace(self.desc, generation=4)))

    def test_rejects_odd_zero_and_overflow_generation(self):
        with self.reader() as reader:
            for generation in (0, 1, 3, -2, 2**64):
                with self.subTest(generation=generation), self.assertRaises(SharedRegionError):
                    reader.read(dataclasses.replace(self.desc, generation=generation))

    def test_row_and_extent_are_one_contract(self):
        with self.reader() as reader:
            for row, offset, length in ((-1, 128, 4), (2, 384, 4), (0, 256, 4),
                                        (0, 64, 4), (0, 128, 65), (0, 128, 0), (0, -1, 4)):
                bad = dataclasses.replace(self.desc, location=PosixSharedLocation("/region", row, offset), byte_length=length)
                with self.subTest(row=row, offset=offset, length=length), self.assertRaises(SharedRegionError):
                    reader.read(bad)
            self.assertIsNone(reader.read(dataclasses.replace(self.desc, byte_length=5)))

    def test_domain_region_and_shape_validation(self):
        with self.reader() as reader:
            for changes in ({"memory_domain": "host"}, {"dimensions": [-1]},
                            {"dimensions": [2], "byte_strides": [4, 8]},
                            {"location": PosixSharedLocation("/other", 0, 128)}):
                with self.subTest(changes=changes), self.assertRaises(SharedRegionError):
                    reader.view(dataclasses.replace(self.desc, **changes))

    def test_shape_and_strides_fit_the_declared_payload(self):
        with self.reader() as reader:
            for changes in ({"element_type": "fp32", "dimensions": [2]},
                            {"element_type": "uint8", "dimensions": [2], "byte_strides": [4]},
                            {"element_type": "unknown", "dimensions": [1]},
                            {"dimensions": [1]}):
                with self.subTest(changes=changes), self.assertRaises(SharedRegionError):
                    reader.read(dataclasses.replace(self.desc, **changes))
            good = dataclasses.replace(self.desc, element_type="uint8", dimensions=[2, 2], byte_strides=[2, 1])
            self.assertEqual(reader.read(good), b"data")
            with self.assertRaises(SharedRegionError):
                reader.view(dataclasses.replace(self.desc, byte_length=5))

    def test_rejects_names_symlinks_and_bad_geometry_before_mapping(self):
        for name in ("/../region", "/nested/region", "/..", "region", "/bad\0name"):
            with self.subTest(name=name), self.assertRaises(SharedRegionError):
                self.reader(dataclasses.replace(self.region, region_name=name))
        os.symlink(self.path, Path(self.tmp.name) / "link")
        with self.assertRaises(OSError):
            self.reader(dataclasses.replace(self.region, region_name="/link"))
        for changes in ({"row_bytes": 64}, {"nof_rows": 0}, {"header_bytes": 0},
                        {"region_bytes": 319}, {"layout_version": 2}, {"memory_domain": "host"}):
            with self.subTest(changes=changes), self.assertRaises(SharedRegionError):
                self.reader(dataclasses.replace(self.region, **changes))
        with self.assertRaises(SharedRegionError):
            self.reader(maximum_bytes=319)

    def test_rejects_truncated_backing_and_corrupt_row_header_size(self):
        self.path.write_bytes(self.data[:64])
        with self.assertRaises(SharedRegionError):
            self.reader()
        REGION_HEADER.pack_into(self.data, 0, REGION_MAGIC, 1, 64, 0, 2, 128, 0, 2)
        self.path.write_bytes(self.data)
        with self.assertRaises(SharedRegionError):
            self.reader()

    def test_borrowed_view_must_be_released_before_close(self):
        reader = self.reader()
        view = reader.view(self.desc)
        try:
            with self.assertRaises(BufferError):
                reader.close()
        finally:
            view.release()
            reader.close()
        with self.assertRaises(SharedRegionError):
            reader.read(self.desc)


if __name__ == "__main__":
    unittest.main()
