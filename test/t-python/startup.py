import sys,unittest
import ldmud

class TestModule(unittest.TestCase):
    def testMaster(self):
        ob = ldmud.Object("/master")
        self.assertEqual(ob, ldmud.get_master())

    def testSimulEfun(self):
        self.assertIsNone(ldmud.get_simul_efun())

class TestObject(unittest.TestCase):
    def testInitLoaded(self):
        ob = ldmud.Object("/master")
        self.assertIsNotNone(ob)

    def testInitLoad(self):
        oldob = ldmud.efuns.find_object("/testob")
        if oldob:
            ldmud.efuns.destruct(oldob)
        ob = ldmud.Object("/testob")
        self.assertIsNotNone(ob)

    def testInitNonExisting(self):
        with self.assertRaises(RuntimeError):
            ob = ldmud.Object("/imnotthere")

class TestArray(unittest.TestCase):
    def testInitEmpty(self):
        arr = ldmud.Array()
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 0)

    def testInitSize(self):
        arr = ldmud.Array(size = 10)
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 10)
        self.assertTrue(0 in arr)
        self.assertFalse(1 in arr)

    def testInitValues(self):
        arr = ldmud.Array([42, 1.5, "Hi"])
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 3)
        self.assertFalse(0 in arr)
        self.assertTrue(42 in arr)
        self.assertTrue(1.5 in arr)
        self.assertTrue("Hi" in arr)

    def testInitIter(self):
        arr = ldmud.Array(range(10))
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 10)
        self.assertTrue(0 in arr)
        self.assertTrue(9 in arr)
        self.assertFalse(10 in arr)

    def testConcat(self):
        arr = ldmud.Array([1,2,3]) + ldmud.Array([4,5,6])
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 6)
        self.assertTrue(1 in arr)
        self.assertTrue(6 in arr)

    def testRepeat(self):
        arr = ldmud.Array([1,2,3]) * 10
        self.assertIsNotNone(arr)
        self.assertEqual(len(arr), 30)
        self.assertTrue(1 in arr)
        self.assertTrue(3 in arr)

    def testItemGetSet(self):
        arr = ldmud.Array(size = 4)
        arr[0] = 10
        arr[1] = 21
        arr[2] = 52
        arr[3] = "Me"
        self.assertEqual(arr[0], 10)
        self.assertEqual(arr[1], 21)
        self.assertEqual(arr[2], 52)
        self.assertEqual(arr[3], "Me")

class TestMapping(unittest.TestCase):
    def testInitEmpty(self):
        m = ldmud.Mapping()
        self.assertIsNotNone(m)
        self.assertEqual(len(m), 0)
        self.assertEqual(m.width, 1)

    def testInitWidth(self):
        m = ldmud.Mapping(width=10)
        self.assertIsNotNone(m)
        self.assertEqual(len(m), 0)
        self.assertEqual(m.width, 10)

    def testInitDict(self):
        m = ldmud.Mapping({1:1, 2:4, 3:9, 4:16})
        self.assertIsNotNone(m)
        self.assertEqual(len(m), 4)
        self.assertEqual(m.width, 1)
        self.assertEqual(m[1], 1)
        self.assertEqual(m[2], 4)
        self.assertEqual(m[3], 9)
        self.assertEqual(m[4], 16)
        self.assertTrue(4 in m)
        self.assertFalse(5 in m)

    def testInitList(self):
        m = ldmud.Mapping([(1,2,3),(4,5,6),(7,8,9),(10,11,12),(13,14,15)])
        self.assertIsNotNone(m)
        self.assertEqual(len(m), 5)
        self.assertEqual(m.width, 2)
        self.assertEqual(m[1], 2)
        self.assertEqual(m[4,1], 6)
        self.assertEqual(m[7,0], 8)
        self.assertTrue(13 in m)
        self.assertFalse(14 in m)

    def testInitInvalid(self):
        with self.assertRaises(ValueError):
            m = ldmud.Mapping([(1,),(2,3)])
        with self.assertRaises(ValueError):
            m = ldmud.Mapping([(), ()])
        with self.assertRaises(ValueError):
            m = ldmud.Mapping(width=-1)

    def testGetSetSimple(self):
        m = ldmud.Mapping()
        self.assertIsNotNone(m)

        m['Hello'] = 1
        m['World'] = 2
        self.assertEqual(len(m), 2)
        self.assertEqual(m.width, 1)
        self.assertEqual(m['Hello'], 1)
        self.assertEqual(m['World'], 2)
        self.assertEqual(m['!'], 0)
        self.assertFalse('!' in m)

    def testDeletion(self):
        m = ldmud.Mapping( { "One": 1, "Two": 2, "Three": 3 } )
        self.assertIsNotNone(m)

        self.assertEqual(len(m), 3)
        self.assertEqual(m.width, 1)
        del m["Two"]

        self.assertEqual(len(m), 2)
        self.assertEqual(m.width, 1)
        self.assertFalse('Two' in m)
        self.assertEqual(m['One'], 1)
        self.assertEqual(m['Three'], 3)

    def testGetSetWide(self):
        m = ldmud.Mapping(width=2)
        self.assertIsNotNone(m)

        m['Hello',0] = 1
        m['Hello',1] = 42
        m['World'] = 2
        m['World',1] = 100

        self.assertEqual(len(m), 2)
        self.assertEqual(m.width, 2)
        self.assertEqual(m['Hello'], 1)
        self.assertEqual(m['Hello',0], 1)
        self.assertEqual(m['Hello',1], 42)
        self.assertEqual(m['World'], 2)
        self.assertEqual(m['World',0], 2)
        self.assertEqual(m['World',1], 100)

    def testGetSetInvalid(self):
        m = ldmud.Mapping(width=0)
        with self.assertRaises(Exception):
            m[10] = 1
        with self.assertRaises(Exception):
            val = m[10]

        m = ldmud.Mapping(width=2)
        with self.assertRaises(IndexError):
            m['Hi','There'] = 'Me'
        with self.assertRaises(IndexError):
            m['Hi',2] = 0

class TestStruct(unittest.TestCase):
    def setUp(self):
        self.master = ldmud.efuns.find_object("/master")

    def testInitSimple(self):
        s = ldmud.Struct(self.master, "test_struct")
        self.assertIsNotNone(s)

    def testInitValueTuple(self):
        s = ldmud.Struct(self.master, "test_struct", (42, 1.5, 'Hi',))
        self.assertIsNotNone(s)
        self.assertEqual(s.t_int, 42)
        self.assertEqual(s.t_float, 1.5)
        self.assertEqual(s.t_string, 'Hi')

    def testInitValueList(self):
        s = ldmud.Struct(self.master, "test_struct", [42, 1.5, 'Hi'])
        self.assertIsNotNone(s)
        self.assertEqual(s.t_int, 42)
        self.assertEqual(s.t_float, 1.5)
        self.assertEqual(s.t_string, 'Hi')

    def testInitValueMap(self):
        s = ldmud.Struct(self.master, "test_struct", { 't_int': 42, 't_float': 1.5, 't_string': 'Hi'})
        self.assertIsNotNone(s)
        self.assertEqual(s.t_int, 42)
        self.assertEqual(s.t_float, 1.5)
        self.assertEqual(s.t_string, 'Hi')

    def testSetValue(self):
        s = ldmud.Struct(self.master, "test_struct")
        self.assertIsNotNone(s)
        s.t_int = 123
        s.t_float = 5.5
        s.t_string = 'Hello!'
        self.assertEqual(s.t_int, 123)
        self.assertEqual(s.t_float, 5.5)
        self.assertEqual(s.t_string, 'Hello!')

class TestClosure(unittest.TestCase):
    def setUp(self):
        self.master = ldmud.get_master()

    def testEfun(self):
        s = ldmud.Closure(self.master, "this_object")
        self.assertIsNotNone(s)
        self.assertEqual(s(), self.master)

    def testOperator(self):
        s = ldmud.Closure(self.master, ",")
        self.assertIsNotNone(s)
        with self.assertRaises(RuntimeError):
            s()

    def testLfun(self):
        s = ldmud.Closure(self.master, "master_fun", self.master)
        self.assertIsNotNone(s)
        self.assertEqual(s(), 54321)

class TestSymbol(unittest.TestCase):
    def testSymbolInit(self):
        s = ldmud.Symbol("sym")
        self.assertIsNotNone(s)
        self.assertEqual(s.name, "sym")
        self.assertEqual(s.quotes, 1)

    def testSymbolInitQuotes(self):
        s = ldmud.Symbol("sym", 5)
        self.assertIsNotNone(s)
        self.assertEqual(s.name, "sym")
        self.assertEqual(s.quotes, 5)

    def testSymbolEq(self):
        s1 = ldmud.Symbol("sym", 2)
        s2 = ldmud.Symbol("sym", 2)
        s3 = ldmud.Symbol("sym", 5)
        s4 = ldmud.Symbol("sym2", 2)
        self.assertIsNotNone(s1)
        self.assertIsNotNone(s2)
        self.assertIsNotNone(s3)
        self.assertIsNotNone(s4)
        self.assertEqual(s1, s2)
        self.assertNotEqual(s1, s3)
        self.assertNotEqual(s1, s4)

class TestQuotedArray(unittest.TestCase):
    def testQuotedArrayInit(self):
        qa = ldmud.QuotedArray(ldmud.Array([2, 1]))
        self.assertIsNotNone(qa)
        self.assertEqual(len(qa.array), 2)
        self.assertEqual(qa.array[0], 2)
        self.assertEqual(qa.array[1], 1)
        self.assertEqual(qa.quotes, 1)

    def testQuotedArrayInitQuotes(self):
        qa = ldmud.QuotedArray(ldmud.Array([2, 1]), quotes=5)
        self.assertIsNotNone(qa)
        self.assertEqual(len(qa.array), 2)
        self.assertEqual(qa.array[0], 2)
        self.assertEqual(qa.array[1], 1)
        self.assertEqual(qa.quotes, 5)

    def testQuotedArrayInitQuotedArray(self):
        qa1 = ldmud.QuotedArray(ldmud.Array([4, 3]), quotes=2)
        qa2 = ldmud.QuotedArray(qa1, quotes=3)
        self.assertIsNotNone(qa2)
        self.assertEqual(len(qa2.array), 2)
        self.assertEqual(qa2.array[0], 4)
        self.assertEqual(qa2.array[1], 3)
        self.assertEqual(qa2.quotes, 5)

    def testQuotedArrayInitInvalid(self):
        with self.assertRaises(TypeError):
            ldmud.QuotedArray(1.5, 1)

class TestEfuns(unittest.TestCase):
    def testDir(self):
        self.assertGreater(len(dir(ldmud.efuns)), 200)

    def testCalls(self):
        master = ldmud.efuns.find_object("/master")
        self.assertEqual(ldmud.efuns.call_other(master, "master_fun"), 54321)
        self.assertEqual(ldmud.efuns.object_name(master), "/master")

def python_test():
    """Run the python test cases."""

    suite = unittest.TestLoader().loadTestsFromName(__name__)
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    return result.wasSuccessful()

def python_return(val):
    """Return the given value to check the
    type conversion between Python and LPC."""
    return val

# Test of the garbage collection.
lpc_value = None
def python_set(val):
    """Remember the value <val>."""
    global lpc_value
    lpc_value = val

def python_get():
    """Return the remembered value."""
    return lpc_value

ldmud.register_efun("python_test", python_test)
ldmud.register_efun("python_return", python_return)
ldmud.register_efun("python_get", python_get)
ldmud.register_efun("python_set", python_set)

ldmud.register_efun("abs", lambda x: x*2)
ldmud.register_efun("unregister_abs", lambda: ldmud.unregister_efun("abs"))

# Test of the hooks
num_hb = 0
def hb_hook():
    global num_hb
    num_hb += 1

ob_list = []
def ob_created(ob):
    ob_list.append(ob)

def get_hook_info():
    return ldmud.Array((num_hb, ldmud.Array(ob_list),))

ldmud.register_hook(ldmud.ON_HEARTBEAT, hb_hook)
ldmud.register_hook(ldmud.ON_OBJECT_CREATED, ob_created)

ldmud.register_efun("python_get_hook_info", get_hook_info)
