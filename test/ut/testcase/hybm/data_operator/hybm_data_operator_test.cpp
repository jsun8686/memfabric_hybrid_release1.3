/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include "hybm_data_op_sdma.h"

class HybmDataOperatorTest : public testing::Test {
public:
    void SetUp() override {};

    void TearDown() override
    {
        GlobalMockObject::verify();
        GlobalMockObject::reset();
    };
};

TEST_F(HybmDataOperatorTest, update_gva_space_test)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_BUTT, 1, 1, 1);
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 1, 0, 1);
}

TEST_F(HybmDataOperatorTest, clean_up_test)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::CleanUp();
}

TEST_F(HybmDataOperatorTest, update_gva_space_unknown_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_BUTT, 0, 0, 0);
}

TEST_F(HybmDataOperatorTest, update_gva_space_device_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 0x1000, 0x100000, 2); // 2
}

TEST_F(HybmDataOperatorTest, update_gva_space_host_type)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_HOST, 0x2000, 0x200000, 4); // 4
}

TEST_F(HybmDataOperatorTest, update_gva_space_zero_size)
{
    ock::mf::DataOperatorPtr opPtr = std::make_shared<ock::mf::HostDataOpSDMA>();
    opPtr->ock::mf::DataOperator::UpdateGvaSpace(HYBM_MEM_TYPE_DEVICE, 0x1000, 0, 2); // 2
}
