#include <Python.h>
#include <structmember.h>
#include <bytesobject.h>

#include <math.h>
#include <stdlib.h>

#include <petsc.h>

#include <quac.h>
#include <operators.h>
#include <error_correction.h>
#include <solver.h>
#include <dm_utilities.h>
#include <quantum_circuits.h>
#include <quantum_gates.h>
#include <qasm_parser.h>
#include <qsystem.h>

static int quac_initialized = 0;

static void finalize_atexit(void) {
  if (quac_initialized)
    QuaC_finalize();
}

static PyObject *
quac_finalize(PyObject *self, PyObject *args) {
  QuaC_finalize();
  quac_initialized = 0;
  Py_RETURN_NONE;
}

static PyObject *
quac_initialize(PyObject *self, PyObject *args) {
  int argc;
  char **argv;
  PyObject* argl;

  argl = PySys_GetObject((char *)("argv"));
  argc = 1;
  if (argl && PyList_Size(argl) > 0)
    argc = (int) PyList_GET_SIZE(argl);

  argv = (char **) malloc(sizeof(char *)*argc);
  argv[0] = (char *) "quac";
  for (int i = 0; i < argc; ++i)
    argv[i] =
#if PY_MAJOR_VERSION < 3
      PyString_AS_STRING(PyList_GET_ITEM(argl, i));
#else
      (char *) PyUnicode_AS_DATA(PyList_GET_ITEM(argl, i));
#endif

  QuaC_initialize(argc, argv);
  quac_initialized = 1;

  (void) Py_AtExit(finalize_atexit);

  free(argv);
  Py_RETURN_NONE;
}

static PyObject *
quac_clear(PyObject *self, PyObject *args) {
  QuaC_clear();
  Py_RETURN_NONE;
}

static PetscErrorCode ts_monitor(TS, PetscInt, PetscReal, Vec, void*);

static _Complex double time_dep_cb(double, void*);

typedef struct {
  PyObject_HEAD

  qsystem system;

  int nid, np;

  PetscInt num_qubits;
  int num_levels;
  operator *qubits;
  qvec rho;

  PyObject *ts_monitor_callback;
} QuaCInstance;

typedef struct {
  PyObject_HEAD
  circuit c;
} QuaCCircuit;

static void
QuaCCircuit_dealloc(QuaCCircuit *self) {
  Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *
QuaCCircuit_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  QuaCCircuit *self;
  self = (QuaCCircuit *) type->tp_alloc(type, 0);
  if (self == NULL)
    return (PyObject *) self;

  memset(&self->c, 0, sizeof(circuit));

  return (PyObject *) self;
}

static int
QuaCCircuit_init(QuaCCircuit *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = {"start_time", NULL};
  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|d", kwlist,
                                   &self->c.start_time))
    return -1;

  return 0;
}

static PyMemberDef QuaCCircuit_members[] = {
    {"num_gates", T_LONG, offsetof(QuaCCircuit, c.num_gates), READONLY,
     "number of gates"},
    {"start_time", T_DOUBLE, offsetof(QuaCCircuit, c.start_time), 0,
     "start time"},
    {NULL}  /* Sentinel */
};

static PyObject *
QuaCCircuit_repr(QuaCCircuit * self) {
  PyObject *r;
  char *st = PyOS_double_to_string(self->c.start_time, 'g', 0, 0, NULL);
  r = PyUnicode_FromFormat("<QuaC Curcuit{%ld gates starting at t=%s}>",
                           self->c.num_gates, st);
  PyMem_Free(st);
  return r;
}

static PyObject *
QuaCCircuit_read_qasm(QuaCCircuit *self, PyObject *args, PyObject *kwds) {
  char *filename, *format;
  PetscInt num_qubits;

  static char *kwlist[] = {"format", "filename", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "ss", kwlist,
                                   &format, &filename))
    Py_RETURN_NONE;

  if (!strcasecmp(format, "quil")) {
    quil_read(filename, &num_qubits, &self->c);
  } else if (!strcasecmp(format, "projectq")) {
    projectq_qasm_read(filename, &num_qubits, &self->c);
  } else if (!strcasecmp(format, "qiskit")) {
    qiskit_qasm_read(filename, &num_qubits, &self->c);
  } else {
    PyErr_SetString(PyExc_RuntimeError, "Unknown qasm format!");
    Py_RETURN_NONE;
  }

  return PyLong_FromLong(num_qubits);
}

static PyObject *
QuaCCircuit_init2(QuaCCircuit *self, PyObject *args, PyObject *kwds) {
  PetscInt num_gates = 0;

  static char *kwlist[] = {"num_gates", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|l", kwlist,
                                   &num_gates))
    Py_RETURN_NONE;

  create_circuit(&self->c, num_gates);

  Py_RETURN_NONE;
}

static PyObject *
QuaCCircuit_add_gate(QuaCCircuit *self, PyObject *args, PyObject *kwds) {
  int qubit1 = -1, qubit2 = -1;
  PetscReal theta = 0, phi = 0, lambda = 0, time = 0;
  gate_type gate;
  char *gate_name;

  static char *kwlist[] = {"gate", "qubit1", "qubit2", "theta", "phi", "lam", "time", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "si|idddd", kwlist,
                                   &gate_name, &qubit1, &qubit2, &theta, &phi, &lambda, &time))
   Py_RETURN_NONE;

  if (!strcasecmp(gate_name, "CZX")) {
    gate = CZX;
  } else if (!strcasecmp(gate_name, "CmZ")) {
    gate = CmZ;
  } else if (!strcasecmp(gate_name, "CZ")) {
    gate = CZ;
  } else if (!strcasecmp(gate_name, "CXZ")) {
    gate = CXZ;
  } else if (!strcasecmp(gate_name, "CNOT")) {
    gate = CNOT;
  } else if (!strcasecmp(gate_name, "H")) {
    gate = HADAMARD;
  } else if (!strcasecmp(gate_name, "X")) {
    gate = SIGMAX;
  } else if (!strcasecmp(gate_name, "Y")) {
    gate = SIGMAY;
  } else if (!strcasecmp(gate_name, "Z")) {
    gate = SIGMAZ;
  } else if (!strcasecmp(gate_name, "I")) {
    gate = EYE;
  } else if (!strcasecmp(gate_name, "RX")) {
    gate = RX;
  } else if (!strcasecmp(gate_name, "RY")) {
    gate = RY;
  } else if (!strcasecmp(gate_name, "RZ")) {
    gate = RZ;
  } else if (!strcasecmp(gate_name, "U1")) {
    gate = U1;
  } else if (!strcasecmp(gate_name, "U2")) {
    gate = U2;
  } else if (!strcasecmp(gate_name, "U3")) {
    gate = U3;
  } else {
    PyErr_SetString(PyExc_RuntimeError, "Unknown gate type!");
    Py_RETURN_NONE;
  }

  if (((int) gate) < 0) {
    if (qubit2 < 0) {
      PyErr_SetString(PyExc_RuntimeError, "qubit2 must be specified for a two-qubit gate!");
      Py_RETURN_NONE;
    }

    add_gate_to_circuit_sys(&self->c, time, gate, qubit1, qubit2);
  } else if (gate == U3){
    add_gate_to_circuit_sys(&self->c, time, gate, qubit1, theta, phi, lambda);
  } else if (gate == U2){
    theta = M_PI / 2;
    add_gate_to_circuit_sys(&self->c, time, gate, qubit1, theta, phi, lambda);
  } else if (gate == U1) {
    theta = 0.0;
    phi = 0.0;
    add_gate_to_circuit_sys(&self->c, time, gate, qubit1, theta, phi, lambda);
  } else {
    add_gate_to_circuit_sys(&self->c, time, gate, qubit1, theta);
  }

  Py_RETURN_NONE;
}

static PyMethodDef QuaCCircuit_methods[] = {
    {"initialize_and_read_qasm",
     (PyCFunction) QuaCCircuit_read_qasm, METH_VARARGS | METH_KEYWORDS,
     "Initialize and read QASM from the specified file using the specified format."
    },
    {"initialize",
     (PyCFunction) QuaCCircuit_init2, METH_VARARGS | METH_KEYWORDS,
     "Initialize the circuit object."
    },
    {"add_gate",
     (PyCFunction) QuaCCircuit_add_gate, METH_VARARGS | METH_KEYWORDS,
     "Add a gate to the circuit object."
    },
    {NULL}  /* Sentinel */
};

static PyTypeObject QuaCCircuitType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "quac.Circuit",
  .tp_doc = "QuaC Circuit",
  .tp_basicsize = sizeof(QuaCCircuit),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
  .tp_new = QuaCCircuit_new,
  .tp_init = (initproc) QuaCCircuit_init,
  .tp_dealloc = (destructor) QuaCCircuit_dealloc,
  .tp_members = QuaCCircuit_members,
  .tp_methods = QuaCCircuit_methods,
  .tp_repr = QuaCCircuit_repr,
  .tp_str = QuaCCircuit_repr,
};


static void
QuaCInstance_dealloc(QuaCInstance *self) {
  if (self->qubits) {
    for (int i = 0; i < self->num_qubits; ++i)
      destroy_op(&self->qubits[i]);

    free(self->qubits);
  }

  if (self->rho)
    destroy_qvec(&self->rho);

  Py_XDECREF(self->ts_monitor_callback);
  Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *
QuaCInstance_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
  QuaCInstance *self = NULL;

  if (!quac_initialized) {
    PyErr_SetString(PyExc_RuntimeError, "QuaC must be initialized first!");
    return (PyObject *) self;
  }

  self = (QuaCInstance *) type->tp_alloc(type, 0);
  if (self == NULL)
    return (PyObject *) self;

  self->nid = nid;
  self->np = np;

  initialize_system(&self->system);

  self->num_qubits = 0;
  self->num_levels = 0;
  self->qubits = NULL;
  self->rho = NULL;

  self->ts_monitor_callback = Py_None;
  Py_INCREF(self->ts_monitor_callback);

  return (PyObject *) self;
}

static int
QuaCInstance_init(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  static char *kwlist[] = {"num_qubits", "ts_monitor", NULL};
  PyObject *tsm = NULL, *tmp;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|lO", kwlist,
                                   &self->num_qubits, &tsm))
    return -1;

  if (tsm) {
    tmp = self->ts_monitor_callback;
    Py_INCREF(tsm);
    self->ts_monitor_callback = tsm;
    Py_XDECREF(tmp);
  }

  return 0;
}

static PyMemberDef QuaCInstance_members[] = {
    {"ts_monitor", T_OBJECT_EX, offsetof(QuaCInstance, ts_monitor_callback), 0,
     "time-step-coup_1, monitor callback"},
    {"num_qubits", T_LONG, offsetof(QuaCInstance, num_qubits), 0,
     "number of qubits"},
    {"node_id", T_INT, offsetof(QuaCInstance, nid), READONLY,
     "node (rank) identifier"},
    {"num_nodes", T_INT, offsetof(QuaCInstance, np), READONLY,
     "number of nodes"},
    {NULL}  /* Sentinel */
};

static PyObject *
QuaCInstance_repr(QuaCInstance * self) {
  return PyUnicode_FromFormat("<QuaC Instance{%d qubits; %d levels; node %d of %d}>",
                              self->num_qubits, self->num_levels, self->nid, self->np);
}

static PyObject *
QuaCInstance_create_qubits(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  self->num_levels = 2;

  // TODO: Can we support different numbers of levels for different qudits?

  static char *kwlist[] = {"num_levels", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist,
                                   &self->num_levels))
    Py_RETURN_NONE;

  if (self->qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qbits for this QuaC instance have already been created!");
    Py_RETURN_NONE;
  } else if (self->num_qubits) {
    self->qubits = (operator *) malloc(sizeof(operator)*self->num_qubits);
    for (int i = 0; i < self->num_qubits; ++i)
      create_op_sys(self->system, self->num_levels, &self->qubits[i]);
  }

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_lindblad_emission(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit;
  double gamma_1;

  static char *kwlist[] = {"qubit", "gamma_1", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "id", kwlist,
                                   &qubit, &gamma_1))
    Py_RETURN_NONE;

  add_lin_term(self->system, gamma_1, 1, self->qubits[qubit]);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_lindblad_dephasing(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit;
  double gamma_2;

  static char *kwlist[] = {"qubit", "gamma_2", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "id", kwlist,
                                   &qubit, &gamma_2))
    Py_RETURN_NONE;

  if (qubit >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit index is out of range");
    Py_RETURN_NONE;
  }

  add_lin_term(self->system, gamma_2, 1, self->qubits[qubit]->n);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_lindblad_thermal_coupling(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit;
  double therm_1, n_therm = 0.5;

  static char *kwlist[] = {"qubit", "therm_1", "n_therm", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "id|d", kwlist,
                                   &qubit, &therm_1, &n_therm))
    Py_RETURN_NONE;

  if (qubit >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit index is out of range");
    Py_RETURN_NONE;
  }

  add_lin_term(self->system, therm_1*(n_therm + 1), 1, self->qubits[qubit]);
  add_lin_term(self->system, therm_1*(n_therm), 1, self->qubits[qubit]->dag);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_lindblad_cross_coupling(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit1, qubit2;
  double coup_1;

  static char *kwlist[] = {"qubit1", "qubit2", "coup_1", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "iid", kwlist,
                                   &qubit1, &qubit2, &coup_1))
    Py_RETURN_NONE;

  if (qubit1 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit1 index is out of range");
    Py_RETURN_NONE;
  }

  if (qubit2 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit2 index is out of range");
    Py_RETURN_NONE;
  }

  add_lin_term(self->system, coup_1, 2, self->qubits[qubit1]->dag, self->qubits[qubit2]);
  add_lin_term(self->system, coup_1, 2, self->qubits[qubit1], self->qubits[qubit2]->dag);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_ham_num(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit;
  double coeff;

  static char *kwlist[] = {"qubit", "coeff", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "id", kwlist,
                                   &qubit, &coeff))
    Py_RETURN_NONE;

  if (qubit >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit index is out of range");
    Py_RETURN_NONE;
  }

  add_ham_term(self->system, coeff, 1, self->qubits[qubit]->n);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_ham_cross_coupling(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit1, qubit2;
  double coup_1;

  static char *kwlist[] = {"qubit1", "qubit2", "coup_1", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "iid", kwlist,
                                   &qubit1, &qubit2, &coup_1))
    Py_RETURN_NONE;

  if (qubit1 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit1 index is out of range");
    Py_RETURN_NONE;
  }

  if (qubit2 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit2 index is out of range");
    Py_RETURN_NONE;
  }

  add_ham_term(self->system, coup_1, 2, self->qubits[qubit1]->dag, self->qubits[qubit2]);
  add_ham_term(self->system, coup_1, 2, self->qubits[qubit1], self->qubits[qubit2]->dag);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_ham_num_time_dep(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit;
  PyObject *coeff;

  static char *kwlist[] = {"qubit", "coeff", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO", kwlist,
                                   &qubit, &coeff))
    Py_RETURN_NONE;

  if (qubit >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit index is out of range");
    Py_RETURN_NONE;
  }

  Py_INCREF(coeff);
  add_ham_term_time_dep(self->system, 1.0, time_dep_cb, coeff, 1, self->qubits[qubit]->n);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_add_ham_cross_coupling_time_dep(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  int qubit1, qubit2;
  PyObject *coup_1;

  static char *kwlist[] = {"qubit1", "qubit2", "coup_1", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "iiO", kwlist,
                                   &qubit1, &qubit2, &coup_1))
    Py_RETURN_NONE;

  if (qubit1 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit1 index is out of range");
    Py_RETURN_NONE;
  }

  if (qubit2 >= self->num_qubits) {
    PyErr_SetString(PyExc_RuntimeError, "qubit2 index is out of range");
    Py_RETURN_NONE;
  }

  Py_INCREF(coup_1);
  add_ham_term_time_dep(self->system, 1.0, time_dep_cb, coup_1, 2, self->qubits[qubit1]->dag, self->qubits[qubit2]);
  add_ham_term_time_dep(self->system, 1.0, time_dep_cb, coup_1, 2, self->qubits[qubit1], self->qubits[qubit2]->dag);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_create_dm(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  if (self->rho) {
    PyErr_SetString(PyExc_RuntimeError, "The density matrix for this QuaC instance has already been created!");
    Py_RETURN_NONE;
  }

  create_qvec_sys(self->system, &self->rho);
  add_to_qvec_loc(self->rho, 1.0, 0);
  assemble_qvec(self->rho);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_start_circuit_at(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  double time = 0.0;
  PyObject *cir;

  static char *kwlist[] = {"circuit", "time", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|d", kwlist,
                                   &cir, &time))
    Py_RETURN_NONE;

  if (!PyObject_TypeCheck(cir, &QuaCCircuitType)) {
    PyErr_SetString(PyExc_TypeError, "Circuit is not a QuaC.Circuit object!");
    Py_RETURN_NONE;
  }

  apply_circuit_to_sys(self->system, &((QuaCCircuit *) cir)->c, time);

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_print_density_matrix(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  char *filename = NULL;

  static char *kwlist[] = {"filename", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sd", kwlist,
                                   &filename))
    Py_RETURN_NONE;

  if (!self->rho) {
    PyErr_SetString(PyExc_RuntimeError, "The density matrix for this QuaC instance has not been created!");
    Py_RETURN_NONE;
  }

  if (filename) {
    print_qvec_file(self->rho, filename);
  } else {
    print_qvec(self->rho);
  }

  Py_RETURN_NONE;
}

static PyObject *
QuaCInstance_get_bitstring_probs(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  PetscReal *probs;
  PetscInt nloc, i;

  PyListObject *r;

  if (!self->rho) {
    PyErr_SetString(PyExc_RuntimeError, "The density matrix for this QuaC instance has not been created!");
    Py_RETURN_NONE;
  }

  get_bitstring_probs(self->rho, &nloc, &probs);

  r = PyList_New(nloc);

  for (i=0; i<nloc; ++i)
    PyList_SetItem(r, i, PyFloat_FromDouble(probs[i]));

  free(probs);

  return r;
}

static PyObject *
QuaCInstance_run(QuaCInstance *self, PyObject *args, PyObject *kwds) {
  PetscReal dt = 1.0, start_time = 0.0, end_time;
  PetscInt max_steps = INT_MAX-1;

  static char *kwlist[] = {"end_time", "dt", "start_time", "max_steps", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "d|ddl", kwlist,
                                   &end_time, &dt, &start_time, &max_steps))
    Py_RETURN_NONE;

  if (!self->rho) {
    PyErr_SetString(PyExc_RuntimeError, "The density matrix for this QuaC instance has not been created!");
    Py_RETURN_NONE;
  }

  construct_matrix(self->system);

  set_ts_monitor_ctx(ts_monitor, (void *) self);

  time_step_sys(self->system, self->rho, start_time, end_time, dt, max_steps);

  Py_RETURN_NONE;
}

static PyMethodDef QuaCInstance_methods[] = {
    {"create_qubits",
     (PyCFunction) QuaCInstance_create_qubits, METH_VARARGS | METH_KEYWORDS,
     "Create the qubits."
    },
    {"add_lindblad_emission",
     (PyCFunction) QuaCInstance_add_lindblad_emission, METH_VARARGS | METH_KEYWORDS,
     "Add Lindblad spontaneous-emission term(s)."
    },
    {"add_lindblad_dephasing",
     (PyCFunction) QuaCInstance_add_lindblad_dephasing, METH_VARARGS | METH_KEYWORDS,
     "Add Lindblad dephasing term(s)."
    },
    {"add_lindblad_thermal_coupling",
     (PyCFunction) QuaCInstance_add_lindblad_thermal_coupling, METH_VARARGS | METH_KEYWORDS,
     "Add Lindblad thermal-coupling terms."
    },
    {"add_lindblad_cross_coupling",
     (PyCFunction) QuaCInstance_add_lindblad_cross_coupling, METH_VARARGS | METH_KEYWORDS,
     "Add Lindblad cross_coupling terms."
    },
    {"add_ham_num",
     (PyCFunction) QuaCInstance_add_ham_num, METH_VARARGS | METH_KEYWORDS,
     "Add a Hamiltonian number-operator term."
    },
    {"add_ham_cross_coupling",
     (PyCFunction) QuaCInstance_add_ham_cross_coupling, METH_VARARGS | METH_KEYWORDS,
     "Add Hamiltonian cross_coupling terms."
    },
    {"add_ham_num_time_dep",
     (PyCFunction) QuaCInstance_add_ham_num_time_dep, METH_VARARGS | METH_KEYWORDS,
     "Add a Hamiltonian number-operator term."
    },
    {"add_ham_cross_coupling_time_dep",
     (PyCFunction) QuaCInstance_add_ham_cross_coupling_time_dep, METH_VARARGS | METH_KEYWORDS,
     "Add Hamiltonian cross_coupling terms."
    },
    {"create_density_matrix",
     (PyCFunction) QuaCInstance_create_dm, METH_NOARGS,
     "Create the density matrix."
    },
    {"start_circuit_at",
     (PyCFunction) QuaCInstance_start_circuit_at, METH_VARARGS | METH_KEYWORDS,
     "Registers a circuit to start at the specified time."
    },
    {"run",
     (PyCFunction) QuaCInstance_run, METH_VARARGS | METH_KEYWORDS,
     "Simulate the registered circuits."
    },
    {"print_density_matrix",
     (PyCFunction) QuaCInstance_print_density_matrix, METH_VARARGS | METH_KEYWORDS,
     "Print the density matrix."
    },
    {"get_bitstring_probs",
     (PyCFunction) QuaCInstance_get_bitstring_probs, METH_VARARGS | METH_KEYWORDS,
     "Get the bit-string probabilities."
    },
    {NULL}  /* Sentinel */
};

static PyTypeObject QuaCInstanceType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "quac.Instance",
  .tp_doc = "QuaC Instance",
  .tp_basicsize = sizeof(QuaCInstance),
  .tp_itemsize = 0,
  .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
  .tp_new = QuaCInstance_new,
  .tp_init = (initproc) QuaCInstance_init,
  .tp_dealloc = (destructor) QuaCInstance_dealloc,
  .tp_members = QuaCInstance_members,
  .tp_methods = QuaCInstance_methods,
  .tp_repr = QuaCInstance_repr,
  .tp_str = QuaCInstance_repr,
};

PetscErrorCode ts_monitor(TS ts, PetscInt step, PetscReal time, Vec rho, void *ctx) {
  QuaCInstance *self = (QuaCInstance *) ctx;
  if (self->ts_monitor_callback && self->ts_monitor_callback != Py_None) {
    PyObject *arglist;
    PyObject *result;

    arglist = Py_BuildValue("(Old)", self, step, time);
    result = PyObject_CallObject(self->ts_monitor_callback, arglist);
    Py_DECREF(arglist);

    if (result)
      Py_DECREF(result);
  }

  PetscFunctionReturn(0);
}

_Complex double time_dep_cb(double t, void *ctx) {
  PyObject *tdf = (PyObject *) ctx;
  PyObject *arglist;
  PyObject *result;

  if (tdf == Py_None)
    return 1.0;

  arglist = Py_BuildValue("(d)", t);
  result = PyObject_CallObject(tdf, arglist);
  Py_DECREF(arglist);

  if (result) {
    _Complex double v;
    Py_complex vs = PyComplex_AsCComplex(result);
    memcpy(&v, &vs, sizeof(v));

    Py_DECREF(result);
    return v;
  }

  return 1.0;
}

static PyMethodDef QuaCMethods[] = {
  {"initialize",  quac_initialize, METH_NOARGS,
   "Initialize QuaC."},
  {"finalize",  quac_finalize, METH_NOARGS,
   "Finalize QuaC."},
  {"clear",  quac_clear, METH_NOARGS,
   "Clear QuaC's internal state."},
  {NULL, NULL, 0, NULL}        /* Sentinel */
};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef quacmodule = {
  PyModuleDef_HEAD_INIT,
  "quac",   /* name of module */
  NULL,     /* module documentation, may be NULL */
  -1,       /* size of per-interpreter state of the module,
               or -1 if the module keeps state in global variables. */
  QuaCMethods
};
#endif // PY_MAJOR_VERSION >= 3

#if PY_MAJOR_VERSION >= 3
#define INITERROR return NULL
PyMODINIT_FUNC
PyInit_quac(void)
#else
#define INITERROR return
void
initquac(void)
#endif
{
  PyObject *m;

  if (PyType_Ready(&QuaCInstanceType) < 0)
    INITERROR;

  if (PyType_Ready(&QuaCCircuitType) < 0)
    INITERROR;

#if PY_MAJOR_VERSION >= 3
  m = PyModule_Create(&quacmodule);
#else
  m = Py_InitModule("quac", QuaCMethods);
#endif
  if (m == NULL)
    INITERROR;

  Py_INCREF(&QuaCInstanceType);
  PyModule_AddObject(m, "Instance", (PyObject *) &QuaCInstanceType);

  Py_INCREF(&QuaCCircuitType);
  PyModule_AddObject(m, "Circuit", (PyObject *) &QuaCCircuitType);

#if PY_MAJOR_VERSION >= 3
  return m;
#endif
}

#undef INITERROR