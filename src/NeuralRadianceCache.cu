#include <engine/graphics/NeuralRadianceCache.hpp>
#include <random>
#include <engine/util/Log.hpp>

namespace en
{
	NeuralRadianceCache::NeuralRadianceCache(
		const nlohmann::json& config, 
		uint32_t inputCount, 
		uint32_t outputCount,
		uint32_t batchSize) 
		:
		m_Model(tcnn::create_from_config(inputCount, outputCount, config)),
		m_InputCount(inputCount),
		m_OutputCount(outputCount),
		m_BatchSize(batchSize)
	{
	}

	void NeuralRadianceCache::Init(
		uint32_t inferCount,
		uint32_t trainCount,
		float* dCuInferInput,
		float* dCuInferOutput,
		float* dCuTrainInput,
		float* dCuTrainTarget)
	{
		// Check batch size compatibility
		if (inferCount % m_BatchSize != 0 || trainCount % m_BatchSize != 0)
		{
			Log::Error("NRC batch size is not compatible with infer count or train count", true);
		}

		// Init infer buffers
		uint32_t inferBatchCount = inferCount / m_BatchSize;
		m_InferInputBatches.resize(inferBatchCount);
		m_InferOutputBatches.resize(inferBatchCount);
		for (uint32_t i = 0; i < inferBatchCount; i++)
		{
			const size_t floatInputOffset = i * m_BatchSize * m_InputCount;
			const size_t floatOutputOffset = i * m_BatchSize * m_OutputCount;
			m_InferInputBatches[i] = tcnn::GPUMatrix<float>(&(dCuInferInput[floatInputOffset]), m_InputCount, m_BatchSize);
			m_InferOutputBatches[i] = tcnn::GPUMatrix<float>(&(dCuInferOutput[floatOutputOffset]), m_OutputCount, m_BatchSize);
		}

		// Init train buffers
		uint32_t trainBatchCount = trainCount / m_BatchSize;
		m_TrainInputBatches.resize(trainBatchCount);
		m_TrainTargetBatches.resize(trainBatchCount);
		for (uint32_t i = 0; i < inferBatchCount; i++)
		{
			const size_t floatInputOffset = i * m_BatchSize * m_InputCount;
			const size_t floatTargetOffset = i * m_BatchSize * m_OutputCount;
			m_TrainInputBatches[i] = tcnn::GPUMatrix<float>(&(dCuTrainInput[floatInputOffset]), m_InputCount, m_BatchSize);
			m_TrainTargetBatches[i] = tcnn::GPUMatrix<float>(&(dCuTrainTarget[floatTargetOffset]), m_OutputCount, m_BatchSize);
		}
	}
}
