#include <gtest/gtest.h>

#include <tensorpipe/core/message.h>
#include <torch/torch.h>
#include <torch/csrc/distributed/rpc/utils.h>

#include <memory>
#include <string>
#include <vector>


TEST(TensorpipeSerialize, Base) {
  // Sender serializes
  at::Tensor t1 = torch::ones({1024}, at::ScalarType::Int);
  at::Tensor t2 = torch::ones({1024}, at::ScalarType::Float);
  std::vector<at::Tensor> tensors{t1, t2};
  std::vector<char> payload = {'1', '2', '3'};
  std::vector<char> payloadCopy = payload; // for testing
  torch::distributed::rpc::MessageType mtype =
      torch::distributed::rpc::MessageType::UNKNOWN;
  int64_t mId = 100;
  torch::distributed::rpc::Message sendingRpcMessage(
      std::move(payload), std::move(tensors), mtype);
  sendingRpcMessage.setId(mId);
  torch::distributed::rpc::TensorPipeEntry tpEntry =
      torch::distributed::rpc::tensorpipeSerialize(sendingRpcMessage);
  tensorpipe::Message sendingTpMessage = std::move(tpEntry.message);
  EXPECT_EQ(sendingTpMessage.tensors.size(), 2);

  // Mimic receiving message descriptor
  tensorpipe::Message recvingTpMessage;
  recvingTpMessage.length = sendingTpMessage.length;
  recvingTpMessage.metadata = sendingTpMessage.metadata;
  recvingTpMessage.tensors.reserve(sendingTpMessage.tensors.size());
  for (auto& tpTensor : sendingTpMessage.tensors) {
    tensorpipe::Message::Tensor t;
    t.length = tpTensor.length;
    t.metadata = tpTensor.metadata;
    recvingTpMessage.tensors.push_back(std::move(t));
  }
  EXPECT_EQ(
      recvingTpMessage.tensors.size(), sendingTpMessage.tensors.size());

  // Mimic readDescriptor() callback:
  // 1. Allocate rpc message
  // 2. Fill pointers to tensorpipe message
  torch::distributed::rpc::Message recvingRpcMessage =
      torch::distributed::rpc::tensorpipeAllocateMessage(recvingTpMessage);
  EXPECT_EQ(
      recvingRpcMessage.tensors().size(), recvingTpMessage.tensors.size());
  recvingTpMessage.data = (uint8_t*)(recvingRpcMessage.payload().data());
  for (int i = 0; i < recvingRpcMessage.tensors().size(); i++) {
    auto& rpcTensor = recvingRpcMessage.tensors()[i];
    auto& tpTensor = recvingTpMessage.tensors[i];
    tpTensor.data = (uint8_t*)(rpcTensor.data_ptr());
  }

  // Mimic tensorpipe data transfer
  for (int i = 0; i < recvingTpMessage.tensors.size(); i++) {
    auto& srcTensor = sendingTpMessage.tensors[i];
    auto& dstTensor = recvingTpMessage.tensors[i];
    memcpy(dstTensor.data, srcTensor.data, srcTensor.length);
  }
  memcpy(recvingTpMessage.data, sendingTpMessage.data, sendingTpMessage.length);
  recvingTpMessage.metadata = sendingTpMessage.metadata;

  // Data is ready
  EXPECT_EQ(mtype, recvingRpcMessage.type());
  EXPECT_EQ(payloadCopy, recvingRpcMessage.payload());
  EXPECT_EQ(mId, recvingRpcMessage.id());
  EXPECT_TRUE(torch::equal(t1, recvingRpcMessage.tensors()[0]));
  EXPECT_TRUE(torch::equal(t2, recvingRpcMessage.tensors()[1]));
}

TEST(TensorpipeSerialize, RecopySparseTensors) {
  // Take a 1K row of a 1M tensors, and make sure we don't send across 1M rows.
  constexpr size_t k1K = 1024;
  at::Tensor main = torch::randn({k1K, k1K});
  at::Tensor tiny = main.select(0, 2); // Select a row in the middle
  EXPECT_EQ(tiny.numel(), k1K);
  EXPECT_EQ(tiny.storage().numel(), k1K * k1K);

  std::vector<at::Tensor> tensors{main, tiny};
  std::vector<char> payload = {'1', '2', '3'};
  torch::distributed::rpc::MessageType mtype =
      torch::distributed::rpc::MessageType::UNKNOWN;
  torch::distributed::rpc::Message sendingRpcMessage(
      std::move(payload), std::move(tensors), mtype);

  torch::distributed::rpc::TensorPipeEntry tpEntry =
      torch::distributed::rpc::tensorpipeSerialize(sendingRpcMessage);
  tensorpipe::Message sendingTpMessage = std::move(tpEntry.message);

  EXPECT_EQ(tpEntry.reservedTensors.size(), 2);
  EXPECT_EQ(sendingTpMessage.tensors.size(), 2);
  EXPECT_TRUE(torch::equal(main, tpEntry.reservedTensors[0]));
  EXPECT_TRUE(torch::equal(tiny, tpEntry.reservedTensors[1]));
  // Test cloned storage
  EXPECT_EQ(main.storage().data(), sendingTpMessage.tensors[0].data);
  EXPECT_NE(tiny.storage().data(), sendingTpMessage.tensors[1].data);
  EXPECT_EQ(tiny.element_size() * k1K, sendingTpMessage.tensors[1].length);
}

TEST(TensorpipeSerialize, NoDeleterTensors) {
  std::vector<float> blob1{.8, .2};
  std::vector<float> blob2{.7, .5, .9};
  at::Tensor t1 = torch::from_blob((float*)(blob1.data()), blob1.size());
  at::Tensor t2 = torch::from_blob((float*)(blob2.data()), blob2.size());
  std::vector<at::Tensor> tensors{t1, t2};
  std::vector<char> payload = {'1', '2', '3'};
  torch::distributed::rpc::MessageType mtype =
      torch::distributed::rpc::MessageType::UNKNOWN;
  torch::distributed::rpc::Message sendingRpcMessage(
      std::move(payload), std::move(tensors), mtype);

  torch::distributed::rpc::TensorPipeEntry tpEntry =
      torch::distributed::rpc::tensorpipeSerialize(sendingRpcMessage);
  tensorpipe::Message sendingTpMessage = std::move(tpEntry.message);

  EXPECT_EQ(tpEntry.copiedTensors.size(), 2);
  EXPECT_EQ(sendingTpMessage.tensors.size(), 2);
  EXPECT_EQ(tpEntry.copiedTensors[0].size(), sendingTpMessage.tensors[0].length);
  EXPECT_EQ(tpEntry.copiedTensors[1].size(), sendingTpMessage.tensors[1].length);
  EXPECT_EQ(tpEntry.copiedTensors[0].data(), sendingTpMessage.tensors[0].data);
  EXPECT_EQ(tpEntry.copiedTensors[1].data(), sendingTpMessage.tensors[1].data);
  EXPECT_TRUE(
      memcmp(
          tpEntry.copiedTensors[0].data(),
          t1.storage().data(),
          sendingTpMessage.tensors[0].length) == 0);
  EXPECT_TRUE(
      memcmp(
          tpEntry.copiedTensors[1].data(),
          t2.storage().data(),
          sendingTpMessage.tensors[1].length) == 0);
}
