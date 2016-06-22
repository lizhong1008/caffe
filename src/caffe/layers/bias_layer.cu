#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layers/bias_layer.hpp"
#include "caffe/util/math_functions.hpp"


#ifdef USE_GREENTEA
#include "caffe/greentea/greentea.hpp"
#endif

namespace caffe {

#ifdef USE_CUDA
template <typename Dtype>
__global__ void BiasForward(const int_tp n, const Dtype* in,
    const Dtype* bias, const int_tp bias_dim, const int_tp inner_dim,
    Dtype* out) {
  CUDA_KERNEL_LOOP(index, n) {
    const int_tp bias_index = (index / inner_dim) % bias_dim;
    out[index] = in[index] + bias[bias_index];
  }
}
#endif  // USE_CUDA

template <typename Dtype>
void BiasLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int_tp count = top[0]->count();
  const Dtype* bottom_data = bottom[0]->gpu_data();
  const Dtype* bias_data =
      ((bottom.size() > 1) ? bottom[1] : this->blobs_[0].get())->gpu_data();
  Dtype* top_data = top[0]->mutable_gpu_data();

  if (this->device_->backend() == BACKEND_CUDA) {
#ifdef USE_CUDA
    BiasForward<Dtype>  // NOLINT_NEXT_LINE(whitespace/operators)
    CUDA_KERNEL(CAFFE_GET_BLOCKS(count), CAFFE_CUDA_NUM_THREADS)(
        count, bottom_data, bias_data, bias_dim_, inner_dim_, top_data);
#endif  // USE_CUDA
  } else {
#ifdef USE_GREENTEA
    viennacl::ocl::context &ctx = viennacl::ocl::get_context(
        this->device_->id());
    viennacl::ocl::program &program = this->device_->program();
    viennacl::ocl::kernel &oclk_bias_forward = program.get_kernel(
        CL_KERNEL_SELECT("bias_forward"));
    ClState& clState = Caffe::cl_state();
    ClMemOff<Dtype> buf_bottom = clState.get_buffer_mem(bottom_data);
    ClMemOff<Dtype> buf_top = clState.get_buffer_mem(top_data);
    ClMemOff<Dtype> buf_bias = clState.get_buffer_mem(bias_data);

    viennacl::ocl::enqueue(
        oclk_bias_forward(count, WrapHandle(buf_bottom.memobj, &ctx),
                          WrapHandle(buf_bias.memobj, &ctx), bias_dim_,
                          inner_dim_, WrapHandle(buf_top.memobj, &ctx)),
        ctx.get_queue());
#endif  // USE_GREENTEA
  }
}

template<typename Dtype>
void BiasLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
                                    const vector<bool>& propagate_down,
                                    const vector<Blob<Dtype>*>& bottom) {
  if (propagate_down[0] && bottom[0] != top[0]) {
      const Dtype* top_diff = top[0]->gpu_diff();
      Dtype* bottom_diff = bottom[0]->mutable_gpu_diff();
      caffe_copy(bottom[0]->count(), top_diff, bottom_diff);
    }
    // in-place, we don't need to do anything with the data diff
    const bool bias_param = (bottom.size() == 1);
    if ((!bias_param && propagate_down[1])
        || (bias_param && this->param_propagate_down_[0])) {
      const Dtype* top_diff = top[0]->gpu_diff();
      Dtype* bias_diff = (bias_param ? this->blobs_[0].get() : bottom[1])
          ->mutable_gpu_diff();
      bool accum = bias_param;

      for (int_tp n = 0; n < outer_dim_; ++n) {
        caffe_gpu_gemv(CblasNoTrans, bias_dim_, inner_dim_, Dtype(1), top_diff,
                       bias_multiplier_.gpu_data(), Dtype(accum), bias_diff);
        top_diff += dim_;
        accum = true;
      }
    }
}

INSTANTIATE_LAYER_GPU_FUNCS(BiasLayer);

}  // namespace caffe
