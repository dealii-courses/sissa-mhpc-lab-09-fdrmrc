// Make sure we don't redefine things
#ifndef linear_elasticity_include_file
#define linear_elasticity_include_file

#include <deal.II/base/function.h>
#include <deal.II/base/function_parser.h>
#include <deal.II/base/parameter_acceptor.h>
#include <deal.II/base/parsed_convergence_table.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/timer.h>

#include <deal.II/distributed/grid_refinement.h>
#include <deal.II/distributed/tria.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q_generic.h>
#include <deal.II/fe/fe_values_extractors.h>

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/generic_linear_algebra.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/vector.h>

#include <deal.II/meshworker/copy_data.h>
#include <deal.II/meshworker/scratch_data.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/vector_tools.h>

#include <fstream>
#include <iostream>

#define FORCE_USE_OF_TRILINOS

namespace LA {
#if defined(DEAL_II_WITH_PETSC) && !defined(DEAL_II_PETSC_WITH_COMPLEX) && \
  !(defined(DEAL_II_WITH_TRILINOS) && defined(FORCE_USE_OF_TRILINOS))
  using namespace dealii::LinearAlgebraPETSc;
#  define USE_PETSC_LA
#elif defined(DEAL_II_WITH_TRILINOS)
using namespace dealii::LinearAlgebraTrilinos;
#else
#  error DEAL_II_WITH_PETSC or DEAL_II_WITH_TRILINOS required
#endif
} // namespace LA

// Forward declare the tester class
template<typename Integral>
class LinearElasticityTester;

using namespace dealii;

/**
 * Solve the LinearElasticity problem, with Dirichlet or Neumann boundary conditions, on
 * all geometries that can be generated by the functions in the GridGenerator
 * namespace.
 */
template<int dim>
class LinearElasticity: ParameterAcceptor {
public:
	/**
	 * Constructor. Initialize all parameters, and make sure the class is ready to
	 * run.
	 */
	LinearElasticity();

	void
	print_system_info();

	void
	run();

	void
	initialize(const std::string &filename);

	void
	parse_string(const std::string &par);

	using CopyData = MeshWorker::CopyData<1, 1, 1>;
	using ScratchData = MeshWorker::ScratchData<dim>;

protected:
	void
	assemble_system_one_cell(
			const typename DoFHandler<dim>::active_cell_iterator &cell,
			ScratchData &scratch, CopyData &copy);

	void
	copy_one_cell(const CopyData &copy);

	void
	make_grid();

	void
	refine_grid();

	void
	setup_system();

	void
	assemble_system();

	void
	assemble_system_on_range(
			const typename DoFHandler<dim>::active_cell_iterator &begin,
			const typename DoFHandler<dim>::active_cell_iterator &end);

	void
	solve();
	void
	estimate();
	void
	mark();
	void
	output_results(const unsigned cycle) const;

	MPI_Comm mpi_communicator;

	ConditionalOStream pcout;
	mutable TimerOutput timer;

	const unsigned int n_components;

	parallel::distributed::Triangulation<dim> triangulation;
	std::unique_ptr<FiniteElement<dim>> fe; //if you want to be as generic as possible, just use FiniteElement as template argument, instead of FE_Q, as all the other classes are derivedf from FiniteElement
	std::unique_ptr<MappingQGeneric<dim>> mapping;
	DoFHandler<dim> dof_handler;
	AffineConstraints<double> constraints;

	// Only needed changes for MPI
	IndexSet locally_owned_dofs;
	IndexSet locally_relevant_dofs;

	LA::MPI::SparseMatrix system_matrix;
	LA::MPI::Vector locally_relevant_solution;
	LA::MPI::Vector solution;
	LA::MPI::Vector system_rhs;

	Vector<float> error_per_cell;
	std::string estimator_type = "kelly";
	std::string marking_strategy = "global";
	std::pair<double, double> coarsening_and_refinement_factors = { 0.03, 0.3 };

	//scalar valued
	FunctionParser<dim> pre_refinement;
	FunctionParser<dim> coefficient;

	//vector valued (n_components) functions
	FunctionParser<dim> forcing_term;
	FunctionParser<dim> exact_solution;
	FunctionParser<dim> dirichlet_boundary_condition;
	FunctionParser<dim> neumann_boundary_condition;

	std::string fe_name = "FESystem[FE_Q(1)^d]";
	unsigned int mapping_degree = 1;
	unsigned int n_refinements = 4;
	unsigned int n_refinement_cycles = 1;
	std::string output_filename = "linear_elasticity";
	int number_of_threads = -1;

	std::set<types::boundary_id> dirichlet_ids = { 0 };
	std::set<types::boundary_id> neumann_ids;

	std::string forcing_term_expression = "0; -1"; //gravity
	std::string coefficient_expression = "1";
	std::string exact_solution_expression = "0; 0";
	std::string dirichlet_boundary_conditions_expression = "0; 0";
	std::string neumann_boundary_conditions_expression = "0; 0";
	std::string pre_refinement_expression = "0";
	std::map<std::string, double> constants;

	std::string grid_generator_function = "hyper_cube";
	std::string grid_generator_arguments = "0: 1: true";

	ParsedConvergenceTable error_table;

	bool use_direct_solver = true;

	ParameterAcceptorProxy<ReductionControl> solver_control;
	FEValuesExtractors::Vector velocity; //interpret FEValues as a d-dimnesional vector starting  from component 0

	//Physical parameters for linear elasticity
	double mu = 1.0;
	double lambda = 1.0; //large lambda=> material is roughly incompressible

	template<typename Integral>
	friend class LinearElasticityTester;
};

#endif
