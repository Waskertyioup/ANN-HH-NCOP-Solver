//                                  I B E X
// File        : ibex_Optimizer.cpp
// Author      : Gilles Chabert, Bertrand Neveu
// Copyright   : IMT Atlantique (France)
// License     : See the LICENSE file
// Created     : May 14, 2012
// Last Update : Apr 08, 2019
//============================================================================

#include "ibex_Optimizer.h"
#include "ibex_Timer.h"
#include "ibex_Function.h"
#include "ibex_NoBisectableVariableException.h"
#include "ibex_BxpOptimData.h"
#include "ibex_CovOptimData.h"

#include "ibex_SmearFunction.h"
#include "ibex_ExtendedSystem.h"
#include "ibex_OptimLargestFirst.h"
#include "ibex_ExtendedSystem.h"
#include "ibex_System.h"
#include <time.h>

#define IBEX_OPTIM_BENCHS_DIR "/tests/respaldo"

#include <float.h>
#include <stdlib.h>
#include <iomanip>

using namespace std;

namespace ibex {

/*
 * TODO: redundant with ExtendedSystem.
 */
void Optimizer::write_ext_box(const IntervalVector& box, IntervalVector& ext_box) {
	int i2=0;
	for (int i=0; i<n; i++,i2++) {
		if (i2==goal_var) i2++; // skip goal variable
		ext_box[i2]=box[i];
	}
}

void Optimizer::read_ext_box(const IntervalVector& ext_box, IntervalVector& box) {
	int i2=0;
	for (int i=0; i<n; i++,i2++) {
		if (i2==goal_var) i2++; // skip goal variable
		box[i]=ext_box[i2];
	}
}

Optimizer::Optimizer(int n, Ctc& ctc, Bsc& bsc, LoupFinder& finder,
		CellBufferOptim& buffer, 
		int goal_var, double eps_x, double rel_eps_f, double abs_eps_f) :
                						n(n), goal_var(goal_var),
										ctc(ctc), bsc(bsc), loup_finder(finder), buffer(buffer),
										eps_x(eps_x), rel_eps_f(rel_eps_f), abs_eps_f(abs_eps_f),
										trace(0), timeout(-1), extended_COV(true), anticipated_upper_bounding(true),
										status(SUCCESS),
										uplo(NEG_INFINITY), uplo_of_epsboxes(POS_INFINITY), loup(POS_INFINITY),
										loup_point(IntervalVector::empty(n)), initial_loup(POS_INFINITY), loup_changed(false),
										time(0), nb_cells(0), cov(NULL) {

	if (trace) cout.precision(12);
}


Optimizer::Optimizer(OptimizerConfig& config) :
		n           (config.nb_var()),
		goal_var    (config.goal_var()),
		ctc         (config.get_ctc()),
		bsc         (config.get_bsc()),
		loup_finder (config.get_loup_finder()),
		buffer      (config.get_cell_buffer()),
		eps_x       (config.get_eps_x()),
		rel_eps_f   (config.get_rel_eps_f()),
		abs_eps_f   (config.get_abs_eps_f()),
		trace       (config.get_trace()),
		timeout     (config.get_timeout()),
		extended_COV(config.with_extended_cov()),
		anticipated_upper_bounding(config.with_anticipated_upper_bounding()),
		status(SUCCESS),
		uplo(NEG_INFINITY), uplo_of_epsboxes(POS_INFINITY), loup(POS_INFINITY),
		loup_point(IntervalVector::empty(n)), initial_loup(POS_INFINITY), loup_changed(false),
		time(0), nb_cells(0), cov(NULL) {

}

Optimizer::~Optimizer() {
	if (cov) delete cov;
}

// compute the value ymax (decreasing the loup with the precision)
// the heap and the current box are contracted with y <= ymax
double Optimizer::compute_ymax() {
	if (anticipated_upper_bounding) {
		//double ymax = loup - rel_eps_f*fabs(loup); ---> wrong :the relative precision must be correct for ymax (not loup)
		double ymax = loup>0 ?
				1/(1+rel_eps_f)*loup
		:
				1/(1-rel_eps_f)*loup;

		if (loup - abs_eps_f < ymax)
			ymax = loup - abs_eps_f;
		//return ymax;
		return next_float(ymax);
	} else
		return loup;
}

bool Optimizer::update_loup(const IntervalVector& box, BoxProperties& prop) {

	try {

		pair<IntervalVector,double> p=loup_finder.find(box,loup_point,loup,prop);
		loup_point = p.first;
		loup = p.second;

		if (trace) {
			cout << "                    ";
			cout << "\033[32m loup= " << loup << "\033[0m" << endl;
//			cout << " loup point=";
//			if (loup_finder.rigorous())
//				cout << loup_point << endl;
//			else
//				cout << loup_point.lb() << endl;
		}
		return true;

	} catch(LoupFinder::NotFound&) {
		return false;
	}
}

//bool Optimizer::update_entailed_ctr(const IntervalVector& box) {
//	for (int j=0; j<m; j++) {
//		if (entailed->normalized(j)) {
//			continue;
//		}
//		Interval y=sys.ctrs[j].f.eval(box);
//		if (y.lb()>0) return false;
//		else if (y.ub()<=0) {
//			entailed->set_normalized_entailed(j);
//		}
//	}
//	return true;
//}

void Optimizer::update_uplo() {
	double new_uplo=POS_INFINITY;

	if (! buffer.empty()) {
		new_uplo= buffer.minimum();
		if (new_uplo > loup && uplo_of_epsboxes > loup) {
			cout << " loup = " << loup << " new_uplo=" << new_uplo <<  " uplo_of_epsboxes=" << uplo_of_epsboxes << endl;
			ibex_error("optimizer: new_uplo>loup (please report bug)");
		}
		if (new_uplo < uplo) {
			cout << "uplo= " << uplo << " new_uplo=" << new_uplo << endl;
			ibex_error("optimizer: new_uplo<uplo (please report bug)");
		}

		// uplo <- max(uplo, min(new_uplo, uplo_of_epsboxes))
		if (new_uplo < uplo_of_epsboxes) {
			if (new_uplo > uplo) {
				uplo = new_uplo;

				if (trace)
					cout << "\033[33m uplo= " << uplo << "\033[0m" << endl;
			}
		}
		else uplo = uplo_of_epsboxes;
	}
	else if (buffer.empty() && loup != POS_INFINITY) {
		// empty buffer : new uplo is set to ymax (loup - precision) if a loup has been found
		new_uplo=compute_ymax(); // not new_uplo=loup, because constraint y <= ymax was enforced
		//    cout << " new uplo buffer empty " << new_uplo << " uplo " << uplo << endl;

		double m = (new_uplo < uplo_of_epsboxes) ? new_uplo :  uplo_of_epsboxes;
		if (uplo < m) uplo = m; // warning: hides the field "m" of the class
		// note: we always have uplo <= uplo_of_epsboxes but we may have uplo > new_uplo, because
		// ymax is strictly lower than the loup.
	}

}

void Optimizer::update_uplo_of_epsboxes(double ymin) {

	// the current box cannot be bisected.  ymin is a lower bound of the objective on this box
	// uplo of epsboxes can only go down, but not under uplo : it is an upperbound for uplo,
	// that indicates a lowerbound for the objective in all the small boxes
	// found by the precision criterion
	assert (uplo_of_epsboxes >= uplo);
	assert(ymin >= uplo);
	if (uplo_of_epsboxes > ymin) {
		uplo_of_epsboxes = ymin;
		if (trace) {
			cout << " unprocessable tiny box: now uplo<=" << setprecision(12) <<  uplo_of_epsboxes << " uplo=" << uplo << endl;
		}
	}
}

void Optimizer::handle_cell(Cell& c) {

	contract_and_bound(c);

	if (c.box.is_empty()) {
		delete &c;
	} else {
		buffer.push(&c);
	}
}

void Optimizer::contract_and_bound(Cell& c) {

	/*======================== contract y with y<=loup ========================*/
	Interval& y=c.box[goal_var];

	double ymax;
	if (loup==POS_INFINITY) ymax = POS_INFINITY;
	// ymax is slightly increased to favour subboxes of the loup
	// TODO: useful with double heap??
	else ymax = compute_ymax()+1.e-15;

	y &= Interval(NEG_INFINITY,ymax);

	if (y.is_empty()) {
		c.box.set_empty();
		return;
	} else {
		c.prop.update(BoxEvent(c.box,BoxEvent::CONTRACT,BitSet::singleton(n+1,goal_var)));
	}

	/*================ contract x with f(x)=y and g(x)<=0 ================*/
	//cout << " [contract]  x before=" << c.box << endl;
	//cout << " [contract]  y before=" << y << endl;

	ContractContext context(c.prop);
	if (c.bisected_var!=-1) {
		context.impact.clear();
		context.impact.add(c.bisected_var);
		context.impact.add(goal_var);
	}

	ctc.contract(c.box, context);
	//cout << c.prop << endl;
	if (c.box.is_empty()) return;

	//cout << " [contract]  x after=" << c.box << endl;
	//cout << " [contract]  y after=" << y << endl;
	/*====================================================================*/

	/*========================= update loup =============================*/

	IntervalVector tmp_box(n);
	read_ext_box(c.box,tmp_box);

	c.prop.update(BoxEvent(c.box,BoxEvent::CHANGE));

	bool loup_ch=update_loup(tmp_box, c.prop);

	// update of the upper bound of y in case of a new loup found
	if (loup_ch) {
		y &= Interval(NEG_INFINITY,compute_ymax());
		c.prop.update(BoxEvent(c.box,BoxEvent::CONTRACT,BitSet::singleton(n+1,goal_var)));
	}

	//TODO: should we propagate constraints again?

	loup_changed |= loup_ch;

	if (y.is_empty()) { // fix issue #44
		c.box.set_empty();
		return;
	}

	/*====================================================================*/
	// Note: there are three different cases of "epsilon" box,
	// - NoBisectableVariableException raised by the bisector (---> see optimize(...)) which
	//   is independent from the optimizer
	// - the width of the box is less than the precision given to the optimizer ("prec" for the original variables
	//   and "goal_abs_prec" for the goal variable)
	// - the extended box has no bisectable domains (if prec=0 or <1 ulp)
	if ((tmp_box.max_diam()<=eps_x && y.diam() <=abs_eps_f) || !c.box.is_bisectable()) {
		update_uplo_of_epsboxes(y.lb());
		c.box.set_empty();
		return;
	}

	// ** important: ** must be done after upper-bounding
	//kkt.contract(tmp_box);

	if (tmp_box.is_empty()) {
		c.box.set_empty();
	} else {
		// the current extended box in the cell is updated
		write_ext_box(tmp_box,c.box);
	}
}

Optimizer::Status Optimizer::optimize(const IntervalVector& init_box, double obj_init_bound) {
	start(init_box, obj_init_bound);
	return optimize();
}


Optimizer::Status Optimizer::optimize(const CovOptimData& data, double obj_init_bound) {
	start(data, obj_init_bound);
	return optimize();
}

Optimizer::Status Optimizer::optimize(const char* cov_file, double obj_init_bound) {
	CovOptimData data(cov_file);
	start(data, obj_init_bound);
	return optimize();
}

void Optimizer::start(const IntervalVector& init_box, double obj_init_bound) {

	srand(12);

	loup=obj_init_bound;

	// Just to initialize the "loup" for the buffer
	// TODO: replace with a set_loup function
	buffer.contract(loup);

	uplo=NEG_INFINITY;
	uplo_of_epsboxes=POS_INFINITY;

	nb_cells=0;

	buffer.flush();

	Cell* root=new Cell(IntervalVector(n+1));

	write_ext_box(init_box, root->box);

	// add data required by the bisector
	bsc.add_property(init_box, root->prop);

	// add data required by the contractor
	ctc.add_property(init_box, root->prop);

	// add data required by the buffer
	buffer.add_property(init_box, root->prop);

	// add data required by the loup finder
	loup_finder.add_property(init_box, root->prop);

	//cout << "**** Properties ****\n" << root->prop << endl;

	loup_changed=false;
	initial_loup=obj_init_bound;

	loup_point = init_box; //.set_empty();
	time=0;

	if (cov) delete cov;
	cov = new CovOptimData(extended_COV? n+1 : n, extended_COV);
	cov->data->_optim_time = 0;
	cov->data->_optim_nb_cells = 0;

	handle_cell(*root);
}

void Optimizer::start(const CovOptimData& data, double obj_init_bound) {

	loup=obj_init_bound;

	// Just to initialize the "loup" for the buffer
	// TODO: replace with a set_loup function
	buffer.contract(loup);

	uplo=data.uplo();
	loup=data.loup();
	loup_point=data.loup_point();
	uplo_of_epsboxes=POS_INFINITY;

	nb_cells=0;

	buffer.flush();

	for (size_t i=loup_point.is_empty()? 0 : 1; i<data.size(); i++) {

		IntervalVector box(n+1);

		if (data.is_extended_space())
			box = data[i];
		else {
			write_ext_box(data[i], box);
			box[goal_var] = Interval(uplo,loup);
			ctc.contract(box);
			if (box.is_empty()) continue;
		}

		Cell* cell=new Cell(box);

		// add data required by the cell buffer
		buffer.add_property(box, cell->prop);

		// add data required by the bisector
		bsc.add_property(box, cell->prop);

		// add data required by the contractor
		ctc.add_property(box, cell->prop);

		// add data required by the loup finder
		loup_finder.add_property(box, cell->prop);

		buffer.push(cell);
	}

	loup_changed=false;
	initial_loup=obj_init_bound;

	time=0;

	if (cov) delete cov;
	cov = new CovOptimData(extended_COV? n+1 : n, extended_COV);
	cov->data->_optim_time = data.time();
	cov->data->_optim_nb_cells = data.nb_cells();
}

Optimizer::Status Optimizer::optimize() {
	Timer timer;


	//CellBufferOptim& buffer2 = &rec(new CellHeap(*ext_sys));



	timer.start();

	update_uplo();

	

	//cout << "buff1:" << buffer;
	//cout << "buff2:" << buffer2;
	//cout << "buff1*:" << &buffer;
	//cout << "buff2*:" << &buffer2;



	try {
	     while (!buffer.empty()) {

			Cell *bis = buffer.top();
			//Cell *bis2 = buffer2.top();




			//cout << bis;
			//cout << bis2;

			//Parametros HH
			//int tamaniominimo = 10;
			//double prec=1e-7;
			double prec=1e-9;

			//////////////////////////////
			//Segmento Editable
			//sudo ./waf install
			//LSmearBatch2TO1200
			//////////////////////////////


						
			//////////////////////////////
			//Recordar descomentar prints
			//////////////////////////////

			//////////////////////////////
			//RH

			int bisec = 0;

			int rd = rand();
			rd = rd % 3;

			if(rd == 0){
				bisec = 1;
			}
			if(rd == 1){
				bisec = 2;
			}
			if(rd == 2){
				bisec = 3;
			}

			// aqui cambiar bisector
			//bisector 1 (no comentar)
			//LSmear
			bisec = 1;

			///////
			//bisector 2
			OptimLargestFirst bso(goal_var,true,prec,0.5);
			bisec = 2;
			

			///////
			//bisector 3
			RoundRobin bsr(prec,0.5);
			bisec = 3;


			
			if (trace >= 2) cout << " current box " << bis->box << endl;

			// base: buffer
			try{

				//bisector 1
				if (bisec == 1){

					pair<Cell*,Cell*> new_cells=bsc.bisect(*bis);

					buffer.pop();
					delete bis; // deletes the cell.
					nb_cells+=2;  // counting the cells handled ( in previous versions nb_cells was the number of cells put into the buffer3 after being handled)

					/*
					if(nb_cells == 50){
						cout << "newfirst:" << *new_cells.first <<endl;
						cout << "newsecond:" << *new_cells.second <<endl;
					}
					*/

					handle_cell(*new_cells.first);
					handle_cell(*new_cells.second);
					
				}


				//bisector 2
				if (bisec == 2){

					pair<Cell*,Cell*> new_cells=bso.bisect(*bis);

					buffer.pop();
					delete bis; // deletes the cell.
					nb_cells+=2;  // counting the cells handled ( in previous versions nb_cells was the number of cells put into the buffer3 after being handled)

					/*
					if(nb_cells == 50){
						cout << "newfirst:" << *new_cells.first <<endl;
						cout << "newsecond:" << *new_cells.second <<endl;
					}
					*/

					handle_cell(*new_cells.first);
					handle_cell(*new_cells.second);

				}


				//bisector 3
				if (bisec == 3){

					pair<Cell*,Cell*> new_cells=bsr.bisect(*bis);

					buffer.pop();
					delete bis; // deletes the cell.
					nb_cells+=2;  // counting the cells handled ( in previous versions nb_cells was the number of cells put into the buffer3 after being handled)

					/*
					if(nb_cells == 50){
						cout << "newfirst:" << *new_cells.first <<endl;
						cout << "newsecond:" << *new_cells.second <<endl;
					}
					*/

					handle_cell(*new_cells.first);
					handle_cell(*new_cells.second);

				}


				//////////////////////////////
				///// Fin de Modificaciones
				//////////////////////////////


				if (uplo_of_epsboxes == NEG_INFINITY) {
					break;
				}
				if (loup_changed) {
					// In case of a new upper bound (loup_changed == true), all the boxes
					// with a lower bound greater than (loup - goal_prec) are removed and deleted.
					// Note: if contraction was before bisection, we could have the problem
					// that the current cell is removed by contractHeap. See comments in
					// older version of the code (before revision 284).

					double ymax=compute_ymax();
					buffer.contract(ymax);

					//cout << " now buffer is contracted and min=" << buffer.minimum() << endl;

					// TODO: check if happens. What is the return code in this case?
					if (ymax <= NEG_INFINITY) {
						if (trace) cout << " infinite value for the minimum " << endl;
						break;
					}
				}
				update_uplo();

				if (!anticipated_upper_bounding) // useless to check precision on objective if 'true'
					if (get_obj_rel_prec()<rel_eps_f || get_obj_abs_prec()<abs_eps_f)
						break;
				if (timeout>0) timer.check(timeout); // TODO: not reentrant, JN: done
				time = timer.get_time();

			}
			//base: buffer
			catch (NoBisectableVariableException& ) {
				update_uplo_of_epsboxes((bis->box)[goal_var].lb());
				buffer.pop();
				delete bis; // deletes the cell.
				update_uplo(); // the heap has changed -> recalculate the uplo (eg: if not in best-first search)

			}


			
			
		}

		// End optim

	 	timer.stop();
	 	time = timer.get_time();

		// No solution found and optimization stopped with empty buffer
		// before the required precision is reached => means infeasible problem
	 	if (uplo_of_epsboxes == NEG_INFINITY)
	 		status = UNBOUNDED_OBJ;
	 	else if (uplo_of_epsboxes == POS_INFINITY && (loup==POS_INFINITY || (loup==initial_loup && abs_eps_f==0 && rel_eps_f==0)))
	 		status = INFEASIBLE;
	 	else if (loup==initial_loup)
	 		status = NO_FEASIBLE_FOUND;
	 	else if (get_obj_rel_prec()>rel_eps_f && get_obj_abs_prec()>abs_eps_f)
	 		status = UNREACHED_PREC;
	 	else
	 		status = SUCCESS;
	}
	catch (TimeOutException& ) {
		status = TIME_OUT;
	}

	/* TODO: cannot retrieve variable names here. */
	for (int i=0; i<(extended_COV ? n+1 : n); i++)
		cov->data->_optim_var_names.push_back(string(""));

	cov->data->_optim_optimizer_status = (unsigned int) status;
	cov->data->_optim_uplo = uplo;
	cov->data->_optim_uplo_of_epsboxes = uplo_of_epsboxes;
	cov->data->_optim_loup = loup;

	//cout << "Valor: " << loup;

	cov->data->_optim_time += time;
	cov->data->_optim_nb_cells += nb_cells;
	cov->data->_optim_loup_point = loup_point;

	// for conversion between original/extended boxes
	IntervalVector tmp(extended_COV ? n+1 : n);

	// by convention, the first box has to be the loup-point.
	if (extended_COV) {
		write_ext_box(loup_point, tmp);
		tmp[goal_var] = Interval(uplo,loup);
		cov->add(tmp);
	}
	else {
		cov->add(loup_point);
	}

	while (!buffer.empty()) {
		Cell* cell=buffer.top();
		if (extended_COV)
			cov->add(cell->box);
		else {
			read_ext_box(cell->box,tmp);
			cov->add(tmp);
		}
		delete buffer.pop();
	}
	

	return status;
}

//
// Fin funcion
//

namespace {
const char* green() {
#ifndef _WIN32
	return "\033[32m";
#else
	return "";
#endif
}

const char* red(){
#ifndef _WIN32
	return "\033[31m";
#else
	return "";
#endif
}

const char* white() {
#ifndef _WIN32
	return "\033[0m";
#else
	return "";
#endif
}

}


void Optimizer::report() {

	cout<< "c:" << nb_cells << " t:" << time << " v:" << loup << " p:"<< loup_point << endl;
	
}



} // end namespace ibex
