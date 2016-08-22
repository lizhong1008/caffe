
#ifndef MULTI_NODE_FC_THREAD_H_
#define MULTI_NODE_FC_THREAD_H_

#include <vector>

#include "caffe/multi_node/node_env.hpp"
#include "caffe/multi_node/worker_thread.hpp"
#include "caffe/sgd_solvers.hpp"

namespace caffe {

template <typename Dtype>
class ParamBuf {
 public:
  ParamBuf() { platest_param_ = NULL; }
  ~ParamBuf() { }

  // worker threads get latest version of param (unlocked)
  vector<Blob<Dtype>*> *GetParam();

  // Find the associated param of a solver
  vector<Blob<Dtype>*> *FindParam(void *psolver);

  // associate the param with a solver pointer
  // increase reference count of a pointer by 1
  vector<Blob<Dtype>*> *RefParam(void *psolver, int clock);

  // decrease the reference count of a point by 1
  int DeRefParam(void *psolver);

  // get the clock of a solver
  int GetClock(void *psolver) {
    boost::mutex::scoped_lock lock(ref_mutex_);

    unordered_map<void *, int>::iterator iter = psolver_to_clock_.find(psolver);
    CHECK(iter != psolver_to_idx_.end()) << "cannot find index to pointer: "
                                         << psolver;

    return iter->second;
  }

  void RemoveClock(int clock) {
    boost::mutex::scoped_lock lock(ref_mutex_);

    unordered_map<int, int>::iterator iter = clock_to_idx_.find(clock);
    CHECK(iter != clock_to_idx_.end()) << "cannot find clock in map: "
                                       << clock;

    clock_to_idx_.erase(iter);
  }

  void InitParamBuf(const vector<Blob<Dtype>*>& params);

  // create new parameter using a template
  vector<Blob<Dtype>*> *CreateParam(const vector<Blob<Dtype>*>& params);

  // replace the old version of parameters with a new version
  void ReplaceParam(vector<Blob<Dtype>*> *p);

  // find a param with reference count 0
  // return NULL if fail to find
  vector<Blob<Dtype>*> *FindFreeParam();

 protected:
  // a vetor of paramters
  vector<vector<Blob<Dtype>*>* > param_vec_;

  // the reference count of each paramter
  // the param thread can use parameter with ref count 0
  vector<int> ref_cnt_vec_;

  // the latest param generated by parameter thread
  vector<Blob<Dtype>*> *platest_param_;

  // map a param pointer to ref index
  unordered_map<void *, int> pointer_to_idx_;

  // map a solver pointer to ref index
  unordered_map<void *, int> psolver_to_idx_;

  // map solver to clock
  unordered_map<void *, int> psolver_to_clock_;

  // bind a clock to a parameter index
  unordered_map<int, int> clock_to_idx_;

  // mutex which protects reference count
  boost::mutex ref_mutex_;

DISABLE_COPY_AND_ASSIGN(ParamBuf);
};


template <typename Dtype>
class FcWorker : public WorkerThread<Dtype> {
 public:
  FcWorker() { }

  virtual ~FcWorker() { }

  static ParamBuf<Dtype> *GetParamBuf() {
    boost::call_once(once_, CreateParamBuf);

    return pbuf_;
  }

 private:
  static void CreateParamBuf() {
    pbuf_ = new ParamBuf<Dtype>();
  }

 private:
  static ParamBuf<Dtype> *pbuf_;
  static boost::once_flag once_;

DISABLE_COPY_AND_ASSIGN(FcWorker);
};


template <typename Dtype>
class FcThread : public FcWorker<Dtype> {
 public:
  FcThread() {
    clock_ = 0;
    staleness_ = 0;
  }
  virtual ~FcThread() { }

  virtual void Run();

 protected:
  shared_ptr<Msg> FcForward(shared_ptr<Msg> m);
  void FcBackward(shared_ptr<Msg> m,
                  vector<shared_ptr<Msg> > *preplies,
                  bool copy_diff);

  // copy Input data from Message
  void CopyInputDataFromMsg(shared_ptr<Net<Dtype> > fc_net, shared_ptr<Msg> m);

  // Copy Output Diff from Message
  void CopyOutputDiffFromMsg(shared_ptr<Net<Dtype> > fc_net,
                             shared_ptr<Msg> m);

  virtual void ProcessMsg(shared_ptr<Msg> m);

 protected:
  // the clock of the node
  int clock_;

  // staleness of allowed clock
  int staleness_;

  // buffer the messages with larger clock
  vector<shared_ptr<Msg> > msg_buf_;

DISABLE_COPY_AND_ASSIGN(FcThread);
};


// the last part of FC layers
template <typename Dtype>
class FcLossThread : public FcThread<Dtype> {
 public:
  FcLossThread() { }
  virtual ~FcLossThread() { }

 protected:
  virtual void ProcessMsg(shared_ptr<Msg> m);

 protected:
  static boost::atomic_int iter_;

DISABLE_COPY_AND_ASSIGN(FcLossThread);
};


// for updating FC parameters
template <typename Dtype>
class FcParamThread : public FcWorker<Dtype> {
 public:
  FcParamThread(int fc_threads) {
    train_iter_ = 0;
    test_node_id_ = -1;
    total_omp_threads_ = 0;
    fc_threads_ = fc_threads;
    num_conv_workers_ = NodeEnv::Instance()->num_workers();
    num_sub_solvers_ = NodeEnv::Instance()->num_sub_solvers();
    sub_batches_ = 0;
    sub_loss_ = 0;
  }

  virtual void Run();

 protected:
  void SendParam(shared_ptr<Msg> m);

  void UpdateParam(shared_ptr<Msg> m);

  virtual Solver<Dtype> *CreateSolver(const Solver<Dtype> *root_solver,
                                      const SolverParameter& solver_param) {
    return NULL;
  }

  void SendNotify();

  int GetGroupIndex(void *psolver, int64_t msg_id);

  void ClearGroup(int grp_idx);

  // send clock update to workers
  void UpdateClock();

 protected:
  // map a clock to vector index where the solver is stored
  unordered_map<int, int> clock_to_group_idx_;

  // a group solver collects all the gradients of the same clock
  vector<void *> group_solvers_;

  // number of gradients updates in a solver
  vector<int> grad_updates_vec_;

  // the loss of the clock group
  vector<Dtype> group_loss_vec_;

  // the msg id of the group solver
  vector<int64_t> msg_id_vec_;

  // the clock of the group of solvers
  vector<int> clock_vec_;

  int train_iter_;

  int test_node_id_;

  // number of conv. clients
  int num_conv_workers_;

  // number of fc threads
  int fc_threads_;

  // total number of omp threads the process has
  int total_omp_threads_;

  // number of overlapping solvers
  int num_sub_solvers_;

  // number of sub batches the param thread processed
  int sub_batches_;

  // sum of loss the param thread processed
  Dtype sub_loss_;

DISABLE_COPY_AND_ASSIGN(FcParamThread);
};

}  // end namespace caffe

#endif


