#include "linear_elasticity.h"

#include <deal.II/base/multithread_info.h>
#include <deal.II/base/thread_management.h>
#include <deal.II/base/work_stream.h>
#include <deal.II/fe/fe_tools.h>

#include <deal.II/grid/grid_out.h>
#include <deal.II/grid/grid_refinement.h>

#include <deal.II/lac/sparse_direct.h>
#include <deal.II/lac/trilinos_precondition.h>

#include <deal.II/meshworker/copy_data.h>
#include <deal.II/meshworker/scratch_data.h>

#include <deal.II/numerics/error_estimator.h>

using namespace dealii;

template<int dim>
LinearElasticity<dim>::LinearElasticity() :
		mpi_communicator(MPI_COMM_WORLD), pcout(std::cout,
				(Utilities::MPI::this_mpi_process(mpi_communicator) == 0)), timer(
				pcout, TimerOutput::summary, TimerOutput::cpu_and_wall_times), n_components(
				dim), triangulation(mpi_communicator,
				typename Triangulation<dim>::MeshSmoothing(
						Triangulation<dim>::smoothing_on_refinement
								| Triangulation<dim>::smoothing_on_coarsening)), dof_handler(
				triangulation), forcing_term(n_components), exact_solution(
				n_components), dirichlet_boundary_condition(n_components), neumann_boundary_condition(
				n_components), error_table(
				std::vector<std::string>(n_components, "u")), solver_control(
				"Solver control", 1000, 1e-12, 1e-12), velocity(0) {
	TimerOutput::Scope timer_section(timer, "constructor");
	add_parameter("Finite element space", fe_name);
	add_parameter("Mapping degree", mapping_degree);
	add_parameter("Number of global refinements", n_refinements);
	add_parameter("Output filename", output_filename);
	add_parameter("Forcing term expression", forcing_term_expression);
	add_parameter("Dirichlet boundary condition expression",
			dirichlet_boundary_conditions_expression);
	add_parameter("Coefficient expression", coefficient_expression);
	add_parameter("Number of threads", number_of_threads);
	add_parameter("Exact solution expression", exact_solution_expression);
	add_parameter("Neumann boundary condition expression",
			neumann_boundary_conditions_expression);

	add_parameter("Local pre-refinement grid size expression",
			pre_refinement_expression);

	add_parameter("Dirichlet boundary ids", dirichlet_ids);
	add_parameter("Neumann boundary ids", neumann_ids);

	add_parameter("Problem constants", constants);
	add_parameter("Grid generator function", grid_generator_function);
	add_parameter("Grid generator arguments", grid_generator_arguments);
	add_parameter("Number of refinement cycles", n_refinement_cycles);

	add_parameter("Estimator type", estimator_type, "", this->prm,
			Patterns::Selection("exact|kelly|residual"));

	add_parameter("Marking strategy", marking_strategy, "", this->prm,
			Patterns::Selection("global|fixed_fraction|fixed_number"));

	add_parameter("Coarsening and refinement factors",
			coarsening_and_refinement_factors);

	add_parameter("Use direct solver", use_direct_solver);
	add_parameter("Linear elasticity mu", mu);
	add_parameter("Linear elasticity lambda", lambda);

	this->prm.enter_subsection("Error table");
	error_table.add_parameters(this->prm);
	this->prm.leave_subsection();
}

template<int dim>
void LinearElasticity<dim>::initialize(const std::string &filename) {
	TimerOutput::Scope timer_section(timer, "initialize");
	ParameterAcceptor::initialize(filename, "last_used_parameters.prm",
			ParameterHandler::Short);
}

template<int dim>
void LinearElasticity<dim>::parse_string(const std::string &parameters) {
	TimerOutput::Scope timer_section(timer, "parse_string");
	ParameterAcceptor::prm.parse_input_from_string(parameters);
	ParameterAcceptor::parse_all_parameters();
}

template<int dim>
void LinearElasticity<dim>::make_grid() {
	TimerOutput::Scope timer_section(timer, "make_grid");

	constants["mu"] = mu;
	constants["lambda"] = lambda;

	const auto vars = dim == 1 ? "x" : dim == 2 ? "x,y" : "x,y,z";
	pre_refinement.initialize(vars, pre_refinement_expression, constants);
	GridGenerator::generate_from_name_and_arguments(triangulation,
			grid_generator_function, grid_generator_arguments);

	for (unsigned int i = 0; i < n_refinements; ++i) {
		for (const auto &cell : triangulation.active_cell_iterators())
			if (pre_refinement.value(cell->center()) < cell->diameter())
				cell->set_refine_flag();
		triangulation.execute_coarsening_and_refinement();
	}

	std::cout << "Number of active cells: " << triangulation.n_active_cells()
			<< std::endl;
}

template<int dim>
void LinearElasticity<dim>::refine_grid() {
	TimerOutput::Scope timer_section(timer, "refine_grid");
	// Cells have been marked in the mark() method.
	triangulation.execute_coarsening_and_refinement();
}

template<int dim>
void LinearElasticity<dim>::setup_system() {
	TimerOutput::Scope timer_section(timer, "setup_system");
	if (!fe) {
		fe = FETools::get_fe_by_name<dim>(fe_name);
		mapping = std::make_unique < MappingQGeneric < dim >> (mapping_degree);
		const auto vars = dim == 1 ? "x" : dim == 2 ? "x,y" : "x,y,z";
		forcing_term.initialize(vars, forcing_term_expression, constants);
		coefficient.initialize(vars, coefficient_expression, constants);
		exact_solution.initialize(vars, exact_solution_expression, constants);

		dirichlet_boundary_condition.initialize(vars,
				dirichlet_boundary_conditions_expression, constants);

		neumann_boundary_condition.initialize(vars,
				neumann_boundary_conditions_expression, constants);
	}

	dof_handler.distribute_dofs(*fe);

	locally_owned_dofs = dof_handler.locally_owned_dofs();
	DoFTools::extract_locally_relevant_dofs(dof_handler, locally_relevant_dofs);

	pcout << "Number of degrees of freedom: " << dof_handler.n_dofs()
			<< std::endl;

	constraints.clear();
	constraints.reinit(locally_relevant_dofs);
	DoFTools::make_hanging_node_constraints(dof_handler, constraints);

	for (const auto &id : dirichlet_ids)
		VectorTools::interpolate_boundary_values(*mapping, dof_handler, id,
				dirichlet_boundary_condition, constraints);
	constraints.close();

	DynamicSparsityPattern dsp(dof_handler.n_dofs());
	DoFTools::make_sparsity_pattern(dof_handler, dsp, constraints, false);
	SparsityTools::distribute_sparsity_pattern(dsp, locally_owned_dofs,
			mpi_communicator, locally_relevant_dofs);

	system_matrix.reinit(locally_owned_dofs, locally_owned_dofs, dsp,
			mpi_communicator);

	solution.reinit(locally_owned_dofs, mpi_communicator);
	system_rhs.reinit(locally_owned_dofs, mpi_communicator);

	locally_relevant_solution.reinit(locally_owned_dofs, locally_relevant_dofs,
			mpi_communicator);

	error_per_cell.reinit(triangulation.n_active_cells());
}

template<int dim>
void LinearElasticity<dim>::assemble_system_one_cell(
		const typename DoFHandler<dim>::active_cell_iterator &cell,
		ScratchData &scratch, CopyData &copy) {
	auto &cell_matrix = copy.matrices[0];
	auto &cell_rhs = copy.vectors[0];

	cell->get_dof_indices(copy.local_dof_indices[0]);

	const auto &fe_values = scratch.reinit(cell);
	cell_matrix = 0;
	cell_rhs = 0;

	for (const unsigned int q_index : fe_values.quadrature_point_indices()) {
		for (const unsigned int i : fe_values.dof_indices()) {
			const auto eps_v = fe_values[velocity].symmetric_gradient(i,
					q_index); // SymmetricTensor<2,dim>
			const auto div_v = fe_values[velocity].divergence(i, q_index); // double

			for (const unsigned int j : fe_values.dof_indices()) {
				const auto eps_u = fe_values[velocity].symmetric_gradient(j,
						q_index); // SymmetricTensor<2,dim>
				const auto div_u = fe_values[velocity].divergence(j, q_index); // double

				cell_matrix(i, j) += (mu * scalar_product(eps_v, eps_u)
						+ lambda * div_u * div_v) * fe_values.JxW(q_index); // dx
			}
			for (const unsigned int i : fe_values.dof_indices()) {
				const auto comp_i = this->fe->system_to_component_index(i).first;
				cell_rhs(i) += (fe_values.shape_value(i, q_index) * // phi_i(x_q)
						this->forcing_term.value(
								fe_values.quadrature_point(q_index), comp_i) * // f(x_q)
						fe_values.JxW(q_index));           // dx
			}
		}
	}

	if (cell->at_boundary())
		//  for(const auto face: cell->face_indices())
		for (unsigned int f = 0; f < GeometryInfo<dim>::faces_per_cell; ++f)
			if (neumann_ids.find(cell->face(f)->boundary_id())
					!= neumann_ids.end()) {
				auto &fe_face_values = scratch.reinit(cell, f);
				for (const unsigned int q_index : fe_face_values.quadrature_point_indices())
					for (const unsigned int i : fe_face_values.dof_indices())
						cell_rhs(i) += fe_face_values.shape_value(i, q_index)
								* neumann_boundary_condition.value(
										fe_face_values.quadrature_point(
												q_index))
								* fe_face_values.JxW(q_index);
			}
}

template<int dim>
void LinearElasticity<dim>::copy_one_cell(const CopyData &copy) {
	constraints.distribute_local_to_global(copy.matrices[0], copy.vectors[0],
			copy.local_dof_indices[0], system_matrix, system_rhs);
}

template<int dim>
void LinearElasticity<dim>::assemble_system_on_range(
		const typename DoFHandler<dim>::active_cell_iterator &begin,
		const typename DoFHandler<dim>::active_cell_iterator &end) {
	QGauss<dim> quadrature_formula(fe->degree + 1);
	QGauss<dim - 1> face_quadrature_formula(fe->degree + 1);

	ScratchData scratch(*mapping, *fe, quadrature_formula,
			update_values | update_gradients | update_quadrature_points
					| update_JxW_values, face_quadrature_formula,
			update_values | update_quadrature_points | update_JxW_values);

	CopyData copy(fe->n_dofs_per_cell());

	static Threads::Mutex assemble_mutex;

	for (auto cell = begin; cell != end; ++cell) {
		assemble_system_one_cell(cell, scratch, copy);
		assemble_mutex.lock();
		copy_one_cell(copy);
		assemble_mutex.unlock();
	}
}

template<int dim>
void LinearElasticity<dim>::assemble_system() {
	TimerOutput::Scope timer_section(timer, "assemble_system");
	QGauss<dim> quadrature_formula(fe->degree + 1);
	QGauss<dim - 1> face_quadrature_formula(fe->degree + 1);

	ScratchData scratch(*mapping, *fe, quadrature_formula,
			update_values | update_gradients | update_quadrature_points
					| update_JxW_values, face_quadrature_formula,
			update_values | update_quadrature_points | update_JxW_values);

	CopyData copy(fe->n_dofs_per_cell());

	for (const auto &cell : dof_handler.active_cell_iterators())
		if (cell->is_locally_owned()) {
			assemble_system_one_cell(cell, scratch, copy);
			copy_one_cell(copy);
		}

	system_matrix.compress(VectorOperation::add);
	system_rhs.compress(VectorOperation::add);
}

template<int dim>
void LinearElasticity<dim>::solve() {
	TimerOutput::Scope timer_section(timer, "solve");
	// if (use_direct_solver == true)
	//   {
	//     SparseDirectUMFPACK system_matrix_inverse;
	//     system_matrix_inverse.initialize(system_matrix);
	//     system_matrix_inverse.vmult(solution, system_rhs);
	//   }
	// else
	//   {
	SolverCG<LA::MPI::Vector> solver(solver_control);
	LA::MPI::PreconditionAMG amg;
	amg.initialize(system_matrix);
	solver.solve(system_matrix, solution, system_rhs, amg);
	constraints.distribute(solution);

	locally_relevant_solution = solution;
}

template<int dim>
void LinearElasticity<dim>::estimate() {
	TimerOutput::Scope timer_section(timer, "estimate");
	if (estimator_type == "exact") {
		error_per_cell = 0;
		QGauss<dim> quad(fe->degree + 1);
		VectorTools::integrate_difference(*mapping, dof_handler,
				locally_relevant_solution, exact_solution, error_per_cell, quad,
				VectorTools::H1_seminorm);
	} else if (estimator_type == "kelly") {
		std::map<types::boundary_id, const Function<dim>*> neumann;
		for (const auto id : neumann_ids)
			neumann[id] = &neumann_boundary_condition;

		QGauss<dim - 1> face_quad(fe->degree + 1);
		KellyErrorEstimator<dim>::estimate(*mapping, dof_handler, face_quad,
				neumann, locally_relevant_solution, error_per_cell,
				ComponentMask(), &coefficient);
	} else if (estimator_type == "residual") {
		// h_T || f+\Delta u_h ||_0,T
		// + \sum over faces
		// 1/2 (h_F)^{1/2} || [n.\nabla u_h] ||_0,F

		QGauss<dim - 1> face_quad(fe->degree + 1);
		QGauss<dim> quad(fe->degree + 1);

		std::map<types::boundary_id, const Function<dim>*> neumann;
		for (const auto id : neumann_ids)
			neumann[id] = &neumann_boundary_condition;

		KellyErrorEstimator<dim>::estimate(*mapping, dof_handler, face_quad,
				neumann, locally_relevant_solution, error_per_cell,
				ComponentMask(), &coefficient);

		FEValues<dim> fe_values(*mapping, *fe, quad,
				update_hessians | update_JxW_values | update_quadrature_points);

		std::vector<double> local_laplacians(quad.size());

		double residual_L2_norm = 0;

		unsigned int cell_index = 0;
		for (const auto &cell : dof_handler.active_cell_iterators()) {
			fe_values.reinit(cell);

			fe_values.get_function_laplacians(locally_relevant_solution,
					local_laplacians);
			residual_L2_norm = 0;
			for (const auto q_index : fe_values.quadrature_point_indices()) {
				const auto arg = (local_laplacians[q_index]
						+ forcing_term.value(
								fe_values.quadrature_point(q_index)));
				residual_L2_norm += arg * arg * fe_values.JxW(q_index);
			}
			error_per_cell[cell_index] += cell->diameter()
					* std::sqrt(residual_L2_norm);

			++cell_index;
		}
	} else {
		AssertThrow(false, ExcNotImplemented());
	}
	auto global_estimator = error_per_cell.l2_norm();
	error_table.add_extra_column("estimator", [global_estimator]() {
		return global_estimator;
	});
	error_table.error_from_exact(*mapping, dof_handler,
			locally_relevant_solution, exact_solution);
}

template<int dim>
void LinearElasticity<dim>::mark() {
	TimerOutput::Scope timer_section(timer, "mark");
	if (marking_strategy == "global") {
		for (const auto &cell : triangulation.active_cell_iterators())
			cell->set_refine_flag();
	} else if (marking_strategy == "fixed_fraction") {
		parallel::distributed::GridRefinement::refine_and_coarsen_fixed_fraction(
				triangulation, error_per_cell,
				coarsening_and_refinement_factors.second,
				coarsening_and_refinement_factors.first);
	} else if (marking_strategy == "fixed_number") {
		parallel::distributed::GridRefinement::refine_and_coarsen_fixed_number(
				triangulation, error_per_cell,
				coarsening_and_refinement_factors.second,
				coarsening_and_refinement_factors.first);
	} else {
		Assert(false, ExcInternalError());
	}
}

template<int dim>
void LinearElasticity<dim>::output_results(const unsigned cycle) const {
	TimerOutput::Scope timer_section(timer, "output_results");
	DataOut<dim> data_out;
	DataOutBase::VtkFlags flags;
	flags.write_higher_order_cells = true;
	data_out.set_flags(flags);
	data_out.attach_dof_handler(dof_handler);
	std::vector<std::string> names(n_components, "u");
	std::vector<DataComponentInterpretation::DataComponentInterpretation> interpretation(
			n_components,
			DataComponentInterpretation::component_is_part_of_vector);

	data_out.add_data_vector(locally_relevant_solution, names,
			DataOut<dim>::type_dof_data, interpretation);
	// auto interpolated_exact = solution;
	// VectorTools::interpolate(*mapping,
	//                          dof_handler,
	//                          exact_solution,
	//                          interpolated_exact);
	// auto locally_interpolated_exact = interpolated_exact;

	// data_out.add_data_vector(interpolated_exact, "exact");
	data_out.add_data_vector(error_per_cell, "estimator");
	data_out.build_patches(*mapping, std::max(mapping_degree, fe->degree),
			DataOut<dim>::curved_inner_cells);
	std::string fname = output_filename + "_" + std::to_string(cycle) + ".vtu";
	data_out.write_vtu_in_parallel(fname, mpi_communicator);

	GridOut go;
	go.write_mesh_per_processor_as_vtu(triangulation,
			"tria_" + std::to_string(cycle), false, true);
}

template<int dim>
void LinearElasticity<dim>::print_system_info() {
	if (number_of_threads != -1 && number_of_threads > 0)
		MultithreadInfo::set_thread_limit(
				static_cast<unsigned int>(number_of_threads));

	std::cout << "Number of cores  : " << MultithreadInfo::n_cores()
			<< std::endl << "Number of threads: "
			<< MultithreadInfo::n_threads() << std::endl;
}

template<int dim>
void LinearElasticity<dim>::run() {
	print_system_info();
	make_grid();
	for (unsigned int cycle = 0; cycle < n_refinement_cycles; ++cycle) {
		setup_system();
		assemble_system();
		solve();
		estimate();
		output_results(cycle);
		if (cycle < n_refinement_cycles - 1) {
			mark();
			refine_grid();
		}
	}
	if (pcout.is_active())
		error_table.output_table(std::cout);
}

template class LinearElasticity<1> ;
template class LinearElasticity<2> ;
template class LinearElasticity<3> ;
