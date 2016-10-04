import asyncio
import functools
import random
import time
from testing import Client
from testing import default_test_setup
from testing import gen_data
from testing import gen_points
from testing import gen_series
from testing import InsertError
from testing import PoolError
from testing import QueryError
from testing import run_test
from testing import Series
from testing import Server
from testing import ServerError
from testing import SiriDB
from testing import TestBase
from testing import UserAuthError

TIME_PRECISION = 'ms'


class TestInsert(TestBase):
    title = 'Test inserts and response'

    GEN_POINTS = functools.partial(gen_points, n=1, time_precision=TIME_PRECISION)

    async def _test_series(self, client):

        result = await client.query('select * from "series float"')
        self.assertEqual(result['series float'], self.series_float)

        result = await client.query('select * from "series int"')
        self.assertEqual(result['series int'], self.series_int)

        result = await client.query('list series name, length, type, start, end')
        result['series'].sort()
        self.assertEqual(
            result,
            {   'columns': ['name', 'length', 'type', 'start', 'end'],
                'series': [
                    ['series float', 10000, 'float', self.series_float[0][0], self.series_float[-1][0]],
                    ['series int', 10000, 'integer', self.series_int[0][0], self.series_int[-1][0]],
                ]
            })

    async def insert(self, client, series, n, timeout=1):
        for _ in range(n):
            await client.insert_some_series(series, timeout=timeout, points=self.GEN_POINTS)
            await asyncio.sleep(1.0)



    @default_test_setup(2, time_precision=TIME_PRECISION)
    async def run(self):
        await self.client0.connect()

        self.assertEqual(
            await self.client0.insert({}),
            {'success_msg': 'Inserted 0 point(s) successfully.'})

        self.assertEqual(
            await self.client0.insert([]),
            {'success_msg': 'Inserted 0 point(s) successfully.'})

        self.series_float = gen_points(tp=float, n=10000, time_precision=TIME_PRECISION, ts_gap='5m')
        random.shuffle(self.series_float)
        self.series_int = gen_points(tp=int, n=10000, time_precision=TIME_PRECISION, ts_gap='5m')
        random.shuffle(self.series_int)

        self.assertEqual(
            await self.client0.insert({
                'series float': self.series_float,
                'series int': self.series_int
            }), {'success_msg': 'Inserted 20000 point(s) successfully.'})


        self.series_float.sort()
        self.series_int.sort()

        await self._test_series(self.client0)

        with self.assertRaises(InsertError):
            await self.client0.insert('[]')

        with self.assertRaises(InsertError):
            await self.client0.insert('[]')

        with self.assertRaises(InsertError):
            await self.client0.insert([{}])

        with self.assertRaises(InsertError):
            await self.client0.insert({'no points': []})

        with self.assertRaises(InsertError):
            await self.client0.insert({'no points': [[]]})

        with self.assertRaises(InsertError):
            await self.client0.insert([{'name': 'no points', 'points': []}])

        # timestamps should be interger values
        with self.assertRaises(InsertError):
            await self.client0.insert({'invalid ts': [[0.5, 6]]})

        # empty series name is not allowed
        with self.assertRaises(InsertError):
            await self.client0.insert({'': [[1, 0]]})

        # empty series name is not allowed
        with self.assertRaises(InsertError):
            await self.client0.insert([{'name': '', 'points': [[1, 0]]}])

        await self.db.add_replica(self.server1, 0, sleep=3)

        await self.assertIsRunning(self.db, self.client0, timeout=3)

        await self.client1.connect()

        await self._test_series(self.client1)

        # Create some random series and start 25 insert task parallel
        series = gen_series(n=5000)
        tasks = [
            asyncio.ensure_future(
                self.client0.insert_some_series(series, timeout=0, points=self.GEN_POINTS))
            for i in range(25)]
        await asyncio.gather(*tasks)

        # Check the result
        await self.assertSeries(self.client0, series)

        self.client0.close()
        self.client1.close()

        return False


if __name__ == '__main__':
    SiriDB.LOG_LEVEL = 'DEBUG'
    Server.HOLD_TERM = False
    Server.MEM_CHECK = False
    run_test(TestInsert())
