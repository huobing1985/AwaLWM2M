/************************************************************************************************************************
 Copyright (c) 2016, Imagination Technologies Limited and/or its affiliated group companies.
 All rights reserved.

 Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 following conditions are met:
     1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
        following disclaimer.
     2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
        following disclaimer in the documentation and/or other materials provided with the distribution.
     3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
        products derived from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE 
 USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lwm2m_types.h"

// This table must align with lwm2m_types.h:AwaStaticResourceType
static const char * ResourceTypeStrings[] =
{
    "Invalid",
    "Opaque",
    "Integer",
    "Float",
    "Boolean",
    "String",
    "Time",
    "None",
    "ObjectLink",
};

bool Operations_IsResourceTypeExecutable(Operations operation)
{
    return (operation & Operations_E);
}

bool Operations_IsResourceTypeWritable(Operations operation)
{
    return (operation & Operations_W);
}

bool Operations_IsResourceTypeReadable(Operations operation)
{
    return ((operation == Operations_R) || (operation == Operations_RW));
}

bool Operations_Contains(Operations parent, Operations child)
{
    if (Operations_IsResourceTypeReadable(child) && !Operations_IsResourceTypeReadable(parent))
    {
        return false;
    }
    else if (Operations_IsResourceTypeWritable(child) && !Operations_IsResourceTypeWritable(parent))
    {
        return false;
    }
    else if (Operations_IsResourceTypeExecutable(child) && !Operations_IsResourceTypeExecutable(parent))
    {
        return false;
    }
    return true;
}

size_t Lwm2mCore_GetNumberOfResourceTypeStrings(void)
{
    return sizeof(ResourceTypeStrings) / sizeof(ResourceTypeStrings[0]);
}

const char * Lwm2mCore_ResourceTypeToString(AwaStaticResourceType resourceType)
{
    static const char * result = "Unknown ResourceType";
    size_t numEntries = Lwm2mCore_GetNumberOfResourceTypeStrings();

    resourceType += 1; // AwaStaticResourceType starts from -1
    if ((resourceType >= 0) && (resourceType < numEntries))
    {
        result = ResourceTypeStrings[resourceType];
    }
    return result;
}

AwaStaticResourceType Lwm2mCore_ResourceTypeFromString(const char * resourceTypeString)
{
    int i;
    int numResourceTypeStrings = Lwm2mCore_GetNumberOfResourceTypeStrings();
    AwaStaticResourceType resourceType = AwaStaticResourceType_Invalid;
    for (i = 0; i < numResourceTypeStrings; ++i)
    {
        if (strcmp(ResourceTypeStrings[i], resourceTypeString) == 0)
        {
            resourceType = i;
            break;
        }
    }
    return resourceType - 1; // AwaStaticResourceType starts from -1
}
