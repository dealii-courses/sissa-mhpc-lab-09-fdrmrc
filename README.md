#  Lab 09 - Vector valued problems
## Theory and Practice of Finite Elements

**Luca Heltai** <luca.heltai@sissa.it>

Starter code documentation can be accessed here:

https://dealii-courses.github.io/sissa-mhpc-lab-08/

* * * * *

## General Instructions

For each of the point below, extend the `Poisson` class with functions that
perform the indicated tasks, trying to minimize the amount of code you copy and
paste, possibly restructuring existing code by adding arguments to existing
functions, and generating wrappers similar to the `run` method (e.g.,
`run_exercise_3`).

Once you created a function that performs the given task, add it to the
`poisson-tester.cc` file, and make sure all the exercises are run through the
`gtest` executable, e.g., adding a test for each exercise, as in the following
snippet:

```C++
TEST_F(PoissonTester, Exercise3) {
   run_exercise_3();
}
```

By the end of this laboratory, you will have modified your Poisson code to run
in parallel using shared memory parallelization on multiple threads, and you
will have some knowledge of Task based parallelization
## Lab-09

1. Transform your `PoissonProblem` code to a `LinearElasticityProblem` class
(create a copy of the `PoissonProblem` class, and modify your copy)

2. Add a `n_components=dim` argument to the `LinearElasticityProblem` class,
and make sure that all members that need to know the number of components of
the problem (e.g., all classes derived from the `dealii::Function` class) throw
an assertion in Debug mode if the number of components is wrong.

3. Create the finite element space from the parameter file using
`FETools::get_fe_by_name`, and check with an assertion that the number of
components is correct (removing `fe_degree` from the parameters).

4. Add the parameters `mu` and `lambda` to the parameter file, and assemble the
problem `(mu eps u, eps v) + (lambda div u, div v) = (f,v)` where `u` and `v`
are in $H^1_0(\Omega)^{dim}$, and `eps u = 0.5((grad u) + (grad u)^T)`. Make
sure you add `mu` and `lambda` to the problem constants map, so you can use
them in the functions and in the exact soluion.

5. Make sure the output of your code is vector based, by instructing `DataOut`
to output a vector based solution.

6. Create a common class `BaseProblem`, that contains everything that is shared
between `PoissonProblem` and `LinearElasticity`, and make sure both
`PoissonProblem` and `LinearElasticityProblem` are derived from the base class.
Make sure you initialize the `ParameterAcceptor` class explicitly in the
`BaseProblem`, and then in the derived classes.

7. Make sure you create a `LinearElasticityProblemTester` class to test your
problem using gtest, and create pure shear and pure compression/dilation test
cases.