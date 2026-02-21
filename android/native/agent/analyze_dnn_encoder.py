#!/usr/bin/env python3
"""
分析 DnnStateEncoder 的维度映射关系
模拟 DnnStateEncoder (22 -> 16 -> 8) 的行为，分析：
1. 为什么某些输出维度始终为正或负
2. 22 维输入特征对 8 维输出的贡献
3. Bias 和权重分布的影响
"""

import numpy as np
import random
from collections import defaultdict

# 22 维输入特征名称（来自 HandcraftedStateEncoder）
INPUT_FEATURES = [
    "activityNorm",      # 0: Activity hash 归一化
    "widgetNorm",         # 1: Widget 数量归一化 (0-1)
    "actionNorm",         # 2: Action 数量归一化 (0-1)
    "clickableRatio",     # 3: Action/Widget 比例 (0-1)
    "editableRatio",     # 4: 可编辑 Widget 比例 (0-1)
    "textDensity",       # 5: 有文本的 Widget 比例 (0-1)
    "avgTextLength",      # 6: 平均文本长度归一化 (0-1)
    "targetActionRatio",  # 7: 目标 Action 比例 (0-1)
    "avgDepth",          # 8: 平均 Widget 深度归一化 (0-1)
    "scrollActionRatio",  # 9: 滚动 Action 比例 (0-1)
    "longClickRatio",     # 10: 长按 Action 比例 (0-1)
    "avgWidgetArea",      # 11: 平均 Widget 面积归一化 (0-1)
    "widgetAreaVariance", # 12: Widget 面积方差 (0-1)
    "actionTypeDiversity", # 13: Action 类型多样性 (0-1)
    "backActionRatio",   # 14: 返回 Action 比例 (0-1)
    "resourceIDRatio",   # 15: 有 ResourceID 的 Widget 比例 (0-1)
    "contentDescRatio",  # 16: 有 ContentDesc 的 Widget 比例 (0-1)
    "scrollableWidgetRatio", # 17: 可滚动 Widget 比例 (0-1)
    "buttonRatio",       # 18: Button Widget 比例 (0-1)
    "imageViewRatio",   # 19: ImageView Widget 比例 (0-1)
    "avgAspectRatio",   # 20: 平均宽高比 (0-1)
    "edgeWidgetRatio",  # 21: 边缘 Widget 比例 (0-1)
]

def relu(x):
    return max(0, x)

def tanh(x):
    return np.tanh(x)

class DnnStateEncoderSimulator:
    """模拟 DnnStateEncoder 的行为（seed=42）"""
    
    def __init__(self, seed=42):
        self.seed = seed
        random.seed(seed)
        np.random.seed(seed)
        
        # 初始化权重（与 C++ 代码一致）
        self.W1 = np.random.uniform(-0.5, 0.5, (16, 22))  # 16x22
        self.b1 = np.random.uniform(-0.5, 0.5, 16)
        self.W2 = np.random.uniform(-0.5, 0.5, (8, 16))   # 8x16
        self.b2 = np.random.uniform(-0.5, 0.5, 8)
        
    def forward(self, x):
        """前向传播：22 -> 16 -> 8"""
        # 第一层：22 -> 16 (ReLU)
        h = np.zeros(16)
        for i in range(16):
            s = self.b1[i]
            for j in range(22):
                s += self.W1[i, j] * x[j]
            h[i] = relu(s)
        
        # 第二层：16 -> 8 (tanh)
        out = np.zeros(8)
        for i in range(8):
            s = self.b2[i]
            for j in range(16):
                s += self.W2[i, j] * h[j]
            out[i] = tanh(s)
        
        return out
    
    def analyze_bias_effect(self):
        """分析 bias 对输出的影响"""
        print("=" * 60)
        print("1. Bias 分析")
        print("=" * 60)
        print(f"\n第一层 bias (b1):")
        print(f"  Min: {self.b1.min():.4f}, Max: {self.b1.max():.4f}, Mean: {self.b1.mean():.4f}")
        print(f"  正 bias 数: {np.sum(self.b1 > 0)}, 负 bias 数: {np.sum(self.b1 < 0)}")
        
        print(f"\n第二层 bias (b2):")
        print(f"  Min: {self.b2.min():.4f}, Max: {self.b2.max():.4f}, Mean: {self.b2.mean():.4f}")
        print(f"  正 bias 数: {np.sum(self.b2 > 0)}, 负 bias 数: {np.sum(self.b2 < 0)}")
        
        # 分析第二层 bias 对输出的影响（假设 hidden layer 全为 0）
        zero_hidden_output = np.array([tanh(self.b2[i]) for i in range(8)])
        print(f"\n假设 hidden layer 全为 0 时的输出（仅受 b2 影响）:")
        for i in range(8):
            sign = "+" if zero_hidden_output[i] >= 0 else "-"
            print(f"  Output[{i}]: {zero_hidden_output[i]:7.4f} ({sign})")
        
        return zero_hidden_output
    
    def analyze_weight_distribution(self):
        """分析权重分布"""
        print("\n" + "=" * 60)
        print("2. 权重分布分析")
        print("=" * 60)
        
        print(f"\n第一层权重 (W1, 16x22):")
        print(f"  Min: {self.W1.min():.4f}, Max: {self.W1.max():.4f}, Mean: {self.W1.mean():.4f}, Std: {self.W1.std():.4f}")
        
        print(f"\n第二层权重 (W2, 8x16):")
        print(f"  Min: {self.W2.min():.4f}, Max: {self.W2.max():.4f}, Mean: {self.W2.mean():.4f}, Std: {self.W2.std():.4f}")
        
        # 分析每个输出维度对应的第二层权重
        print(f"\n每个输出维度对应的第二层权重统计:")
        for i in range(8):
            w2_i = self.W2[i, :]
            print(f"  Output[{i}]: mean={w2_i.mean():7.4f}, std={w2_i.std():7.4f}, "
                  f"pos={np.sum(w2_i > 0)}, neg={np.sum(w2_i < 0)}")
    
    def analyze_feature_contribution(self, num_samples=1000):
        """分析输入特征对输出的贡献"""
        print("\n" + "=" * 60)
        print("3. 输入特征贡献分析")
        print("=" * 60)
        
        # 生成随机输入样本（模拟真实 UI 状态）
        samples = []
        for _ in range(num_samples):
            # 模拟真实的特征分布
            x = np.array([
                random.uniform(0, 1),      # activityNorm
                random.uniform(0, 1),      # widgetNorm
                random.uniform(0, 1),      # actionNorm
                random.uniform(0, 1),      # clickableRatio
                random.uniform(0, 0.3),    # editableRatio (通常较小)
                random.uniform(0, 1),      # textDensity
                random.uniform(0, 0.5),    # avgTextLength (归一化后通常较小)
                random.uniform(0, 1),      # targetActionRatio
                random.uniform(0, 0.5),    # avgDepth (归一化后通常较小)
                random.uniform(0, 0.3),    # scrollActionRatio (通常较小)
                random.uniform(0, 0.2),    # longClickRatio (通常很小)
                random.uniform(0, 1),      # avgWidgetArea
                random.uniform(0, 0.5),    # widgetAreaVariance
                random.uniform(0, 1),      # actionTypeDiversity
                random.uniform(0, 0.2),    # backActionRatio (通常很小)
                random.uniform(0, 0.8),    # resourceIDRatio
                random.uniform(0, 0.5),    # contentDescRatio
                random.uniform(0, 0.3),    # scrollableWidgetRatio
                random.uniform(0, 0.4),    # buttonRatio
                random.uniform(0, 0.3),    # imageViewRatio
                random.uniform(0.5, 2.0), # avgAspectRatio (宽高比，通常 > 1)
                random.uniform(0, 0.3),   # edgeWidgetRatio
            ])
            samples.append(x)
        
        # 计算每个输出维度的统计
        output_stats = defaultdict(list)
        for x in samples:
            out = self.forward(x)
            for i in range(8):
                output_stats[i].append(out[i])
        
        print(f"\n基于 {num_samples} 个随机样本的输出统计:")
        for i in range(8):
            values = output_stats[i]
            pos_count = sum(1 for v in values if v >= 0)
            neg_count = sum(1 for v in values if v < 0)
            print(f"  Output[{i}]: min={min(values):7.4f}, max={max(values):7.4f}, "
                  f"mean={np.mean(values):7.4f}, pos={pos_count}/{num_samples} ({100*pos_count/num_samples:.1f}%)")
        
        # 分析输入特征对输出的影响（使用梯度近似）
        print(f"\n输入特征对输出的平均影响（通过权重分析）:")
        # 计算第一层到第二层的间接影响
        feature_impact = np.zeros((8, 22))
        for out_dim in range(8):
            for in_dim in range(22):
                # 计算通过所有 hidden units 的间接影响
                impact = 0.0
                for hidden_dim in range(16):
                    # W1[hidden_dim, in_dim] * W2[out_dim, hidden_dim]
                    # 注意：ReLU 可能截断，这里简化处理
                    impact += self.W1[hidden_dim, in_dim] * self.W2[out_dim, hidden_dim]
                feature_impact[out_dim, in_dim] = impact
        
        # 找出每个输出维度最重要的输入特征
        print(f"\n每个输出维度最重要的输入特征（Top 3）:")
        for i in range(8):
            impacts = feature_impact[i, :]
            top3_indices = np.argsort(np.abs(impacts))[-3:][::-1]
            print(f"  Output[{i}]:")
            for idx in top3_indices:
                print(f"    {INPUT_FEATURES[idx]:20s}: {impacts[idx]:7.4f}")
    
    def analyze_output_patterns(self):
        """分析输出模式（为什么某些维度始终为正或负）"""
        print("\n" + "=" * 60)
        print("4. 输出模式分析")
        print("=" * 60)
        
        # 分析第二层 bias 和权重的组合效果
        print(f"\n分析每个输出维度的 bias + 权重组合:")
        for i in range(8):
            bias = self.b2[i]
            weights = self.W2[i, :]
            
            # 假设 hidden layer 的平均激活（简化：假设 ReLU 后平均为 0.25）
            avg_hidden = 0.25
            avg_contribution = weights.mean() * avg_hidden * 16  # 16 个 hidden units
            
            total_bias = bias + avg_contribution
            expected_sign = "+" if total_bias >= 0 else "-"
            
            print(f"  Output[{i}]:")
            print(f"    Bias: {bias:7.4f}")
            print(f"    Avg weight contribution: {avg_contribution:7.4f}")
            print(f"    Total bias effect: {total_bias:7.4f} ({expected_sign})")
            print(f"    tanh(bias): {tanh(bias):7.4f}")
            print(f"    tanh(total): {tanh(total_bias):7.4f}")

def main():
    print("=" * 60)
    print("DnnStateEncoder 维度映射分析")
    print("=" * 60)
    print(f"\n模拟 DnnStateEncoder (seed=42)")
    print(f"架构: 22 -> 16 (ReLU) -> 8 (tanh)")
    print(f"权重范围: [-0.5, 0.5]")
    print(f"输出范围: [-1, 1] (tanh)\n")
    
    encoder = DnnStateEncoderSimulator(seed=42)
    
    # 1. Bias 分析
    zero_output = encoder.analyze_bias_effect()
    
    # 2. 权重分布
    encoder.analyze_weight_distribution()
    
    # 3. 特征贡献
    encoder.analyze_feature_contribution(num_samples=1000)
    
    # 4. 输出模式
    encoder.analyze_output_patterns()
    
    # 5. 与日志数据对比
    print("\n" + "=" * 60)
    print("5. 与日志数据对比")
    print("=" * 60)
    print("\n从 fastbot4.log 观察到的模式:")
    print("  Head[0]: 始终为负 (-0.73 到 -0.32), avg=-0.50")
    print("  Head[1]: 有正有负 (-0.21 到 0.17), avg=-0.08")
    print("  Head[2]: 始终为正 (0.35 到 0.68), avg=0.47")
    print("  Head[3]: 始终为正 (0.73 到 0.91), avg=0.79 (最大)")
    print("  Head[4]: 有正有负 (-0.52 到 0.37), avg=-0.20")
    print("  Head[5]: 始终为负 (-0.48 到 -0.15), avg=-0.34")
    
    print(f"\n模拟结果（假设 hidden layer 全为 0）:")
    for i in range(6):
        sign_match = "✓" if (zero_output[i] < 0 and i in [0, 5]) or (zero_output[i] > 0 and i in [2, 3]) else "?"
        print(f"  Output[{i}]: {zero_output[i]:7.4f} {sign_match}")
    
    print("\n" + "=" * 60)
    print("结论")
    print("=" * 60)
    print("""
1. Bias 影响：第二层 bias (b2) 直接影响输出符号
   - 如果 b2[i] < 0 且绝对值较大，即使 hidden layer 激活，输出也可能始终为负
   - 如果 b2[i] > 0 且 hidden layer 有正激活，输出可能始终为正

2. 权重分布：权重在 [-0.5, 0.5] 均匀分布，平均接近 0
   - 这意味着如果没有 strong bias，输出应该接近 0
   - 但 tanh 的非线性可能放大某些模式

3. Head[3] 值最大：可能是：
   - b2[3] 较大且为正
   - 或者对应的 W2[3, :] 权重与常见的 hidden layer 激活模式匹配良好

4. Head[0] 和 Head[5] 始终为负：可能是：
   - b2[0] 和 b2[5] 为负且绝对值较大
   - 或者对应的权重模式导致负激活占主导

5. 建议：
   - 如果需要更均衡的输出分布，可以调整 bias 初始化
   - 或者使用真实数据训练 DNN，让模型学习更好的特征表示
    """)

if __name__ == "__main__":
    main()
