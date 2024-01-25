'''
  (C) Copyright 2017-2023 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
'''
import logging
import time
import traceback

from apricot import TestWithServers
from pydaos.raw import DaosApiError, DaosContainer, c_uuid_to_str


class ArrayObjTest(TestWithServers):
    """
    Test Class Description:
    A very simple test verifying the ability to read/write arrays to an object.
    :avocado: recursive
    """
    def test_array_obj(self):
        """
        Test ID: DAOS-961

        Test Description: Writes an array to an object and then reads it
        back and verifies it.

        :avocado: tags=all,daily_regression
        :avocado: tags=vm
        :avocado: tags=object,smoke
        :avocado: tags=ArrayObjTest,test_array_obj
        """
        pool = self.get_pool()

        try:
            # create a container
            container = DaosContainer(self.context)
            container.create(pool.pool.handle)
            self.log.info("Container %s created.", container.get_uuid_str())

            # now open it
            container.open()

            # do a query and compare the UUID returned from create with
            # that returned by query
            container.query()

            if container.get_uuid_str() != c_uuid_to_str(
                    container.info.ci_uuid):
                self.fail("Container UUID did not match the one in info\n")

            # create an object and write some data into it
            thedata = []
            thedata.append(b"data string one")
            thedata.append(b"data string two")
            thedata.append(b"data string tre")
            dkey = b"this is the dkey"
            akey = b"this is the akey"

            self.log.info("writing array to dkey >%s< akey >%s<.", dkey, akey)
            oid = container.write_an_array_value(thedata, dkey, akey, obj_cls=3)

            # read the data back and make sure its correct
            length = len(thedata[0])
            thedata2 = container.read_an_array(len(thedata), length + 1,
                                               dkey, akey, oid)
            if thedata[0][0:length - 1] != thedata2[0][0:length - 1]:
                self.log.error("Data mismatch")
                self.log.error("Wrote: >%s<", thedata[0])
                self.log.error("Read: >%s<", thedata2[0])
                self.fail("Write data, read it back, didn't match\n")

            if thedata[2][0:length - 1] != thedata2[2][0:length - 1]:
                self.log.error("Data mismatch")
                self.log.error("Wrote: >%s<", thedata[2])
                self.log.error("Read: >%s<", thedata2[2])
                self.fail("Write data, read it back, didn't match\n")

            container.close()

            # wait a few seconds and then destroy
            time.sleep(5)
            container.destroy()

            self.log.info("Test Complete")

        except DaosApiError as excep:
            self.log.error("Test Failed, exception was thrown.")
            self.log.error(excep)
            self.log.error(traceback.format_exc())
            self.fail("Test was expected to pass but it failed.\n")
