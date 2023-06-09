#include <Eigen/Sparse>
#include <Engine/MeshEdit/Simulate.h>


using namespace Ubpa;

using namespace std;
using namespace Eigen;


const int log_verbosity = 5;

void Simulate::Clear() {
	this->positions.clear();
	this->velocity.clear();
}

bool Simulate::Init() {
	//Clear();

	// Set all initial velocity to zero
	this->velocity.resize(positions.size());
	for (int i = 0; i < positions.size(); i++)
	{
		for (int j = 0; j < 3; j++)
		{
			this->velocity[i][j] = 0;
		}
	}

	this->mass.resize(positions.size());
	this->f_ext.resize(positions.size());
	for (int i = 0; i < positions.size(); i++) {
		this->mass[i] = 1;
	}

	this->updateExtForce();

	this->adj_map.clear();
	assert(edgelist.size() % 2 == 0);
	for (size_t i = 0; i < edgelist.size() / 2; i++) {
		assert(edgelist[2 * i] != edgelist[2 * i + 1]);
		unsigned small = edgelist[2 * i] < edgelist[2 * i + 1] ? edgelist[2 * i] : edgelist[2 * i + 1];
		unsigned large = edgelist[2 * i] < edgelist[2 * i + 1] ? edgelist[2 * i + 1] : edgelist[2 * i];

		adj_map[make_pair(small, large)] = (positions[edgelist[2 * i]] - positions[edgelist[2 * i + 1]]).norm();

		if (log_verbosity > 50) {
			printf("added edge (%d, %d), len=%lf\n", edgelist[2 * i], edgelist[2 * i + 1]
				, getOrigLen(edgelist[2 * i], edgelist[2 * i + 1]));
		}
	}

	this->updateKMat();

	return true;
}

bool Simulate::Run() {
	SimulateOnceNR();
	//SimulateOnceMin();
	curr_time += h;
	updateExtForce();

	return true;
}

void Ubpa::Simulate::updateKMat() {

	size_t total_points = positions.size();

	// build K_mat
	this->K_mat = SparseMatrix<float>(3 * total_points, 3 * total_points);
	this->K_mat.setZero();

	int current_row = 0;
	for (size_t i = 0; i < total_points; i++) {
		K_mat.insert(current_row, i) = 1;
		current_row++;
	}

	K_mat.makeCompressed();
}


float Simulate::getOrigLen(int i, int j) {
	assert(i != j);
	unsigned small = i < j ? i : j;
	unsigned large = i < j ? j : i;

	float res = adj_map[make_pair(small, large)];
	assert(res > 0);
	return res;
}

float Simulate::getK(int i, int j) {
	return stiff;
}

// i, j are indices in total_points
// with respect to x_i
Eigen::Matrix3f Simulate::calcNablaFintComponent(int i, int j, const std::vector<pointf3>& pos_step_k) {
	Vector3f x_i = Vector3f{ pos_step_k[i][0], pos_step_k[i][1], pos_step_k[i][2] };
	Vector3f x_j = Vector3f{ pos_step_k[j][0], pos_step_k[j][1], pos_step_k[j][2] };

	float dist_norm = (x_i - x_j).norm();
	float l = getOrigLen(i, j);
	float k = getK(i, j);

	Matrix3f f_int_component = k * ((l / dist_norm) - 1) * Matrix3f::Identity()
		- k * l * (1.0 / powf(dist_norm, 3)) * (x_i - x_j) * (x_i - x_j).transpose();

	return f_int_component;
}

// calculating fi(xi,xj)
Eigen::Vector3f Simulate::calcFint(int i, int j, const std::vector<pointf3>& pos_step_k) {
	Vector3f x_i = Vector3f{ pos_step_k[i][0], pos_step_k[i][1], pos_step_k[i][2] };
	Vector3f x_j = Vector3f{ pos_step_k[j][0], pos_step_k[j][1], pos_step_k[j][2] };

	float dist_norm = (x_i - x_j).norm();
	float l = getOrigLen(i, j);
	float k = getK(i, j);

	return (k * (dist_norm - l) / dist_norm) * (x_j - x_i);
}

std::vector<unsigned> Simulate::getAdjV(int vi) {
	// todo improve this, efficiency ugly
	std::vector<unsigned> ret;
	for (int i = 0; i < this->edgelist.size() / 2; i++) {
		if (edgelist[2 * i] == vi) {
			ret.push_back(edgelist[2 * i + 1]);
		}
		else if (edgelist[2 * i + 1] == vi) {
			ret.push_back(edgelist[2 * i]);
		}
	}
	return ret;
}

// K_block * ( g_mid )
float Simulate::calcResidual(const std::vector<vecf3>& f_int, const Eigen::VectorXf& x_new, const std::vector<pointf3>& pos_step_k) {
	// calculate ||g(x) - 0||^2
	size_t n = positions.size();

	VectorXf aggregated = VectorXf::Zero(3 * n);

	for (size_t i = 0; i < n; i++) {
		Vector3f x_n = { this->positions[i][0], this->positions[i][1], this->positions[i][2] };
		Vector3f v_n = { this->velocity[i][0], this->velocity[i][1], this->velocity[i][2] };

		Matrix3f m = Matrix3f::Identity() * mass[i];

		Vector3f y = calcY(i);

		Vector3f fint_total = Vector3f::Zero();
		auto adjv_list = getAdjV(i);
		for (auto adjv : adjv_list) {
			fint_total += calcFint(i, adjv, pos_step_k);
		}

		Vector3f g_mid = m * (Vector3f{ x_new[3 * i], x_new[3 * i + 1], x_new[3 * i + 2] } - y) - h * h * fint_total;

		aggregated(3 * i) = g_mid(0);
		aggregated(3 * i + 1) = g_mid(1);
		aggregated(3 * i + 2) = g_mid(2);
	}

	VectorXf g = K_mat * aggregated;
	return g.norm(); // sqrt(sum(x_i^2))
}



void Simulate::SimulateOnceMin() {


	if (log_verbosity > 50) {
		cout << "Current h: " << h << endl;
	}

	size_t total_points = positions.size();
	size_t free_points = total_points;

	// calc f_int
	std::vector<vecf3> f_int(total_points, vecf3(0, 0, 0));


	// initial value

	VectorXf x_vec(3 * total_points);
	for (size_t i = 0; i < total_points; i++) {
		auto y = calcY(i);
		x_vec(3 * i) = y(0);
		x_vec(3 * i + 1) = y(1);
		x_vec(3 * i + 2) = y(2);
	}

	if (log_verbosity > 10) {
		printf("Residual: %lf\n", calcResidual(vector<vecf3>(), x_vec, this->positions));
	}





	SparseMatrix<float> coeff_mat(3 * total_points, 3 * total_points);
	// the rhs vector
	VectorXf rhs_vec(3 * total_points);

	// the k'th iteration 
	auto positions_k = this->positions;

	for (int iter_count = 0; iter_count < 50; iter_count++) {


		coeff_mat.setZero();
		rhs_vec.setZero();
		// initialize as M quasi-diagonal
		for (int i = 0; i < total_points; i++) {
			coeff_mat.insert(3 * i, 3 * i) = mass[i];
			coeff_mat.insert(3 * i + 1, 3 * i + 1) = mass[i];
			coeff_mat.insert(3 * i + 2, 3 * i + 2) = mass[i];
		}

		// initialize rhs_vec
		for (int i = 0; i < total_points; i++) {


			Vector3f x_n{ positions_k[i][0], positions_k[i][1], positions_k[i][2] };

			// x_n from step k, but Y from step 0
			rhs_vec.block(3 * i, 0, 3, 1) = (-calcG(i, x_n, Vector3f::Zero()))
				+ Matrix3f::Identity() * mass[i] * x_n;
		}

		assert(edgelist.size() % 2 == 0);
		for (size_t i = 0; i < edgelist.size() / 2; i++) {
			// update coeff_mat
			int vi = edgelist[2 * i];
			int vj = edgelist[2 * i + 1];
			assert(getOrigLen(vi, vj) > 0);

			auto f_int_nabla_comp = this->calcNablaFintComponent(vi, vj, positions_k);
			assert(f_int_nabla_comp == this->calcNablaFintComponent(vj, vi, positions_k));

			auto h_squared_f_nabla = h * h * f_int_nabla_comp;
			for (int index_i = 0; index_i < 3; index_i++) {
				for (int index_j = 0; index_j < 3; index_j++) {
					// update nabla_g x_i and x_j
					coeff_mat.coeffRef(3 * vi + index_i, 3 * vi + index_j) += -(h_squared_f_nabla(index_i, index_j));
					coeff_mat.coeffRef(3 * vj + index_i, 3 * vj + index_j) += -(h_squared_f_nabla(index_i, index_j));
				}
			}

			// update rhs_vec

			auto f_int_comp = this->calcFint(vi, vj, positions_k);
			assert(f_int_comp == -calcFint(vj, vi, positions_k));
			auto h_squared_f = h * h * f_int_comp;

			// here "-" * "-" = + (for h_squared_f) !!
			rhs_vec.block(3 * vi, 0, 3, 1) += (h_squared_f)+(-h_squared_f_nabla * Vector3f{ positions_k[vi][0],positions_k[vi][1],positions_k[vi][2] });
			rhs_vec.block(3 * vj, 0, 3, 1) += (-h_squared_f) + (-h_squared_f_nabla * Vector3f{ positions_k[vj][0],positions_k[vj][1],positions_k[vj][2] });
		}

		// solve the problem

		SparseQR<SparseMatrix<float>, COLAMDOrdering<int>> solver;

		cout << "begin makeCompressed()" << endl;
		coeff_mat.makeCompressed();

		if (log_verbosity > 50) {
			cout << coeff_mat << endl;
		}

		solver.compute(coeff_mat);

		cout << "begin compute()" << endl;
		solver.compute(coeff_mat);
		if (solver.info() != Eigen::Success) {
			cout << "solver: decomposition was not successful." << endl;
			return;
		}

		VectorXf x_vec = solver.solve(rhs_vec);

		if (log_verbosity > 50) {
			cout << "rhs_vec: " << endl;
			cout << rhs_vec << endl;

			cout << "x_vec: " << endl;
			cout << x_vec << endl;
		}


		// update x
		for (int i = 0; i < total_points; i++) {
			positions_k[i][0] = x_vec(3 * i);
			positions_k[i][1] = x_vec(3 * i + 1);
			positions_k[i][2] = x_vec(3 * i + 2);

			if (log_verbosity > 50) {
				printf("point %d (%lf, %lf, %lf)\n", i
					, positions_k[i][0]
					, positions_k[i][1]
					, positions_k[i][2]);
			}
		}

		// print residual
		auto residual = calcResidual(vector<vecf3>(), x_vec, positions_k);
		if (log_verbosity > 10) {
			printf("[%d] Residual: %lf\n", iter_count, residual);
		}

		if (fabs(residual) < 1e-3) {
			break;
		}
	}

	// update v and x
	for (int i = 0; i < positions.size(); i++) {
		this->velocity[i] = ((positions_k[i] - positions[i]).cast_to<vecf3>() / h).cast_to<pointf3>();
		this->positions[i] = positions_k[i];
	}

}

void SetBlock(Eigen::SparseMatrix<float>& m, int si, int sj, int r, int c, Eigen::MatrixXf& b)
{
	for (int i = si; i < si + r; ++i) {
		for (int j = sj; j < sj + c; ++j) {
			m.coeffRef(i, j) += b(i - si, j - sj);
		}
	}
}

void Simulate::SimulateOnceNR() {


	if (log_verbosity > 50) {
		cout << "Current h: " << h << endl;
	}

	size_t total_points = positions.size();
	size_t free_points = total_points;

	// calc f
	std::vector<vecf3> f_tot(total_points, vecf3(0, 0, 0));

	for (size_t i = 0; i < this->f_ext.size(); ++i)f_tot[i] = f_ext[i];

	std::vector<vecf3> f_int(total_points, vecf3(0, 0, 0));
	for (int i = 0; i < total_points; ++i) {
		auto adjv_list = getAdjV(i);
		Vector3f tem(0, 0, 0);
		for (auto adjv : adjv_list) {
			tem += calcFint(i, adjv, this->positions);
		}
		for (int j = 0; j < 3; ++j)f_int[i][j] = tem[j];
		f_tot[i] += f_int[i];
	}

	Eigen::VectorXf f(3 * total_points);
	for (int i = 0; i < total_points; ++i) {
		f(3 * i) = f_tot[i][0];
		f(3 * i + 1) = f_tot[i][1];
		f(3 * i + 2) = f_tot[i][2];
	}

	// calc M
	Eigen::SparseMatrix<float> M(3 * total_points, 3 * total_points);
	for (int i = 0; i < total_points; ++i) {
		M.insert(3 * i, 3 * i) = this->mass[i];
		M.insert(3 * i + 1, 3 * i + 1) = this->mass[i];
		M.insert(3 * i + 2, 3 * i + 2) = this->mass[i];
	}

	//calc K
	Eigen::SparseMatrix<float> K(3 * total_points, 3 * total_points);
	for (int i = 0; i < total_points; ++i) {
		auto adjv_list = getAdjV(i);
		for (auto j : adjv_list) {
			Eigen::MatrixXf Kii = calcNablaFintComponent(i, j, this->positions);
			Eigen::MatrixXf Kij = -Kii;
			SetBlock(K, 3 * i, 3 * i, 3, 3, Kii);
			SetBlock(K, 3 * j, 3 * j, 3, 3, Kii);
			SetBlock(K, 3 * i, 3 * j, 3, 3, Kij);
			SetBlock(K, 3 * j, 3 * i, 3, 3, Kij);
		}
	}

	//calc A
	Eigen::SparseMatrix<float> A = M - h * h * K;

	//calc b
	Eigen::VectorXf vt(3 * total_points);
	for (int i = 0; i < total_points; ++i) {
		vt(3 * i) = this->velocity[i][0];
		vt(3 * i + 1) = this->velocity[i][1];
		vt(3 * i + 2) = this->velocity[i][2];
	}
	Eigen::VectorXf b = M * vt + h * f;

	// calc v^(t + 1)
	cout << "begin makeCompressed()" << endl;
	A.makeCompressed();
	SparseQR<SparseMatrix<float>, COLAMDOrdering<int>> solver;
	solver.compute(A);
	if (solver.info() != Eigen::Success) {
		cout << "solver: decomposition was not successful." << endl;
		return;
	}
	VectorXf nv = solver.solve(b);

	// update v
	for (int i = 0; i < total_points; ++i) {
		for (int j = 0; j < 3; ++j)
			this->velocity[i][j] = nv(3 * i + j);
	}

	// update x
	for (int i = 0; i < total_points; ++i) {
		for (int j = 0; j < 3; ++j)
			this->positions[i][j] += this->velocity[i][j] * h;
	}
}
