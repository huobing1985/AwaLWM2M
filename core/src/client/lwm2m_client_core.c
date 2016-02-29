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

#include "lwm2m_core.h"
#include "lwm2m_objects.h"
#include "lwm2m_types.h"
#include "coap_abstraction.h"
#include "lwm2m_object_store.h"
#include "lwm2m_list.h"
#include "lwm2m_tlv.h"
#include "lwm2m_serdes.h"
#include "lwm2m_registration.h"
#include "lwm2m_bootstrap.h"
#include "lwm2m_endpoints.h"
#include "lwm2m_json.h"
#include "lwm2m_plaintext.h"
#include "lwm2m_tree_builder.h"
#include "lwm2m_server_object.h"
#include "lwm2m_result.h"
#include "lwm2m_request_origin.h"
#include "lwm2m_object_tree.h"
#include "lwm2m_security_object.h"
#include "lwm2m_prettyprint.h" /*DEBUG*/


static int Lwm2mCore_ObjectStoreReadHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID,
                                            ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID,
                                            uint8_t * destBuffer, int destBufferLen);

static int Lwm2mCore_ObjectStoreWriteHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID,
                                             ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID,
                                             uint8_t * srcBuffer, int srcBufferLen, bool * changed);

static int Lwm2mCore_ObjectStoreGetLengthHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID,
                                                 ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID);

static int Lwm2mCore_ObjectStoreDeleteHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID);
static int Lwm2mCore_ObjectStoreCreateInstanceHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID);
static int Lwm2mCore_ObjectStoreCreateOptionalResourceHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID);

static int Lwm2mCore_DeviceManagmentEndpointHandler(int type, void * ctxt, AddressType * addr,
                                                    const char * path, const char * query, const char * token, int tokenLength,
                                                    ContentType contentType, const char * requestContent, int requestContentLen,
                                                    ContentType * responseContentType, char * responseContent, int * responseContentLen, int * responseCode);

static Lwm2mContextType Lwm2mContext;

ObjectOperationHandlers defaultObjectOperationHandlers =
{
    .CreateInstance = Lwm2mCore_ObjectStoreCreateInstanceHandler,
    .Delete = Lwm2mCore_ObjectStoreDeleteHandler,
};

ResourceOperationHandlers defaultResourceOperationHandlers =
{
    .Read = Lwm2mCore_ObjectStoreReadHandler,    //default handler, read from object store
    .GetLength = Lwm2mCore_ObjectStoreGetLengthHandler,
    .Write = Lwm2mCore_ObjectStoreWriteHandler,  //default handler, write to object store.
    .Execute = NULL,
    .CreateOptionalResource = Lwm2mCore_ObjectStoreCreateOptionalResourceHandler,
};


// Serialise the Object referenced by OIR into the provided buffer. Return number of bytes serialised, negative on failure
static int Lwm2mCore_SerialiseOIR(Lwm2mTreeNode * root, ContentType acceptContentType, int oir[], int oirLength, ContentType * responseContentType, char * buffer, int size)
{
    int len = -1;

    if (acceptContentType == ContentType_None)
    {
        /* If the content type is not specified in the payload of a response message,
         * the default content type (text/plain) is assumed; otherwise the content type
         * MUST be specified in using one of the supported Media Types.
         */
        acceptContentType = ContentType_ApplicationPlainText;
    }

    if (oirLength == 1)
    {
        len = SerialiseObject(acceptContentType, root, oir[0], buffer, size);
    }
    else if (oirLength == 2)
    {
        len = SerialiseObjectInstance(acceptContentType, root, oir[0], oir[1], buffer, size);
    }
    else if (oirLength == 3)
    {
        len = SerialiseResource(acceptContentType, root, oir[0], oir[1], oir[2], buffer, size);
    }
    else
    {
        Lwm2m_Error("Invalid OIR, length %d\n", oirLength);
    }

    *responseContentType = acceptContentType;
    return len;
}

// Deserialise the encoded buffer provided into the Object references by OIR. Return number of bytes deserialised, negative on failure
static int Lwm2mCore_DeserialiseOIR(Lwm2mTreeNode ** dest, ContentType contentType, Lwm2mContextType * context, int oir[], int oirLength, const char * buffer, int len)
{
    /* If the content type is not specified in the payload of a response message, 
     * the default content type (text/plain) is assumed; otherwise the content type 
     * MUST be specified in using one of the supported Media Types. 
     */
    if (contentType ==  ContentType_None)
    {
        contentType = ContentType_ApplicationPlainText;
    }

    if (oirLength == 1)
    {
        Lwm2m_Debug("Deserialise object %d:\n", oir[0]);
        len = DeserialiseObject(contentType, dest, context->Definitions, oir[0], buffer, len);
    }
    else if (oirLength == 2)
    {
        Lwm2m_Debug("Deserialise object instance %d/%d:\n", oir[0], oir[1]);
        len = DeserialiseObjectInstance(contentType, dest, context->Definitions, oir[0], oir[1], buffer, len);
    }
    else if (oirLength == 3)
    {
        Lwm2m_Debug("Deserialise resource %d/%d/%d:\n", oir[0], oir[1], oir[2]);
        len = DeserialiseResource(contentType, dest, context->Definitions, oir[0], oir[1], oir[2], buffer, len);
    }

    return len;
}

static void Lwm2mCore_ObjectCreated(Lwm2mContextType * context, ObjectIDType objectID)
{
    char path[32];
    sprintf(path, "/%d", objectID);

    Lwm2mCore_AddResourceEndPoint(&context->EndPointList, path, Lwm2mCore_DeviceManagmentEndpointHandler);
    Lwm2mObjectTree_AddObject(&context->ObjectTree, objectID);
    Lwm2m_MarkObserversChanged(context, objectID, -1, -1, NULL, 0);
    Lwm2m_SetUpdateRegistration(context);
}

// This function is called when a new object instance is created
static void Lwm2mCore_ObjectInstanceCreated(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID)
{
    char path[32];
    sprintf(path, "/%d/%d", objectID, objectInstanceID);

    Lwm2mCore_AddResourceEndPoint(&context->EndPointList,path, Lwm2mCore_DeviceManagmentEndpointHandler);
    Lwm2mObjectTree_AddObjectInstance(&context->ObjectTree, objectID, objectInstanceID);
    Lwm2m_MarkObserversChanged(context, objectID, objectInstanceID, -1, NULL, 0);
    Lwm2m_SetUpdateRegistration(context);

    // Must create mandatory resources once the object instance is created
    ObjectDefinition * objectDefinition = Definition_LookupObjectDefinition(context->Definitions, objectID);
    ResourceIDType resourceID = -1;
    while ((resourceID = Definition_GetNextResourceTypeFromObjectType(objectDefinition, resourceID)) != -1)
    {
        ResourceDefinition * resFormat = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
        if (resFormat != NULL)
        {
            if (IS_MANDATORY(resFormat))
            {
                if (Lwm2mCore_CreateOptionalResource(context, objectID, objectInstanceID, resourceID) == -1)
                {
                    Lwm2m_Error("Failed to create optional resource for object %d instance %d resource %d\n", objectID, objectInstanceID, resourceID);
                }
            }
        }
        else
        {
            Lwm2m_Error("No resource definition for object %d resource %d\n", objectID, resourceID);
        }
    }
}

static void Lwm2mCore_ResourceCreated(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    char path[32];
    sprintf(path, "/%d/%d/%d", objectID, objectInstanceID, resourceID);

    Lwm2mCore_AddResourceEndPoint(&context->EndPointList, path, Lwm2mCore_DeviceManagmentEndpointHandler);
    Lwm2mObjectTree_AddResource(&context->ObjectTree, objectID, objectInstanceID, resourceID);

    uint8_t * newValue = NULL;
    int newValueLength = Lwm2mCore_GetResourceInstanceLength(context, objectID, objectInstanceID, resourceID, 0);
    if (newValueLength > 0)
    {
        newValue = malloc(newValueLength);
        if (newValue == NULL)
        {
            Lwm2m_Error("Failed to allocate memory for resource\n");
            return;
        }
        Lwm2mCore_GetResourceInstanceValue(context, objectID, objectInstanceID, resourceID, 0, newValue, newValueLength);
    }
    
    Lwm2m_MarkObserversChanged(context, objectID, objectInstanceID, resourceID, newValue, newValueLength);
    if (newValue)
    {
        free(newValue);
    }
}

// This function is called when a read is performed for a resource that uses the "default" read handler. It is responsible
// for looking up the resource specified and copying it's contents into the buffer specified by "destBuffer".
// Return -1 on error, or the size of the data written to destBuffer.
static int Lwm2mCore_ObjectStoreReadHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID,
                                            ResourceInstanceIDType resourceInstanceID, uint8_t * destBuffer, int destBufferLen)
{
    return ObjectStore_GetResourceInstanceValue(((Lwm2mContextType *)(context))->Store, objectID, objectInstanceID, resourceID, resourceInstanceID, destBuffer, destBufferLen);
}

static int Lwm2mCore_ObjectStoreGetLengthHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID,
                                                     ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID)
{
    return ObjectStore_GetResourceInstanceLength(((Lwm2mContextType *)(context))->Store, objectID, objectInstanceID, resourceID, resourceInstanceID);
}

// This function is called when a write is performed for a resource that uses the "default" write handler. It is responsible
// for looking up the resource specified and populating it from the contents in srcBuffer.
// Return -1 on error, or the size of the data written to destBuffer.
static int Lwm2mCore_ObjectStoreWriteHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID,
                                             ResourceInstanceIDType resourceInstanceID, uint8_t * srcBuffer, int srcBufferLen, bool * changed)
{
    int nullTerminator = 0;

    // If the type is a string, add a NULL terminator.
    if (Definition_GetResourceType(((Lwm2mContextType *)(context))->Definitions, objectID, resourceID) == ResourceTypeEnum_TypeString)
    {
        nullTerminator = 1;
    }
    return ObjectStore_SetResourceInstanceValue(((Lwm2mContextType *)(context))->Store, objectID, objectInstanceID, resourceID,
                                                resourceInstanceID, srcBufferLen + nullTerminator, srcBuffer, 0, srcBufferLen, changed);
}

// This function is called when a delete is performed for an object/object instance that uses the "default" handler.
// Return -1 on error, or 0 on success.
static int Lwm2mCore_ObjectStoreDeleteHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    return ObjectStore_Delete(((Lwm2mContextType *)(context))->Store, objectID, objectInstanceID, resourceID);
}

// This function is called when a create instance is performed for an object that uses the "default" handler.
// Return -1 on error, or the ID of the created object instance on success.
static int Lwm2mCore_ObjectStoreCreateInstanceHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID)
{
    DefinitionRegistry * definitions = ((Lwm2mContextType *)context)->Definitions;
    ObjectStore * store = ((Lwm2mContextType *)context)->Store;
    ObjectDefinition * definition = Definition_LookupObjectDefinition(definitions, objectID);
    if (definition != NULL)
    {
        objectInstanceID = ObjectStore_CreateObjectInstance(store, objectID, objectInstanceID, definition->MaximumInstances);
    }
    else
    {
        Lwm2m_Error("No definition for object ID %d\n", objectID);
        Lwm2mResult_SetResult(Lwm2mResult_NotFound);
        objectInstanceID = -1;
    }
    return objectInstanceID;
}

// Create a new object instance of a specified object type.
// Return -1 on error, or the ID of the created object instance on success.
int Lwm2mCore_CreateObjectInstance(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID)
{
    if ((objectInstanceID != -1) && (Lwm2mCore_Exists(context, objectID, objectInstanceID, -1)))
    {
        Lwm2m_Error("Object instance already exists: /%d/%d\n", objectID, objectInstanceID);
        goto error;
    }

    ObjectDefinition * definition = Definition_LookupObjectDefinition(context->Definitions, objectID);
    if ((definition == NULL) || (definition->Handlers.CreateInstance == NULL))
    {
        Lwm2m_Error("No create handler for object ID %d\n", objectID);
        Lwm2mResult_SetResult(Lwm2mResult_NotFound);
        goto error;
    }

    if ((objectInstanceID = definition->Handlers.CreateInstance(context, objectID, objectInstanceID)) >= 0) 
    {
        Lwm2mCore_ObjectInstanceCreated(context, objectID, objectInstanceID);
    }
    else
    {
        Lwm2m_Error("Could not create Object /%d/%d\n", objectID, objectInstanceID);
    }

    return objectInstanceID;
error:
    return -1;
}

// This function is called when a create optional resource is performed for a resource that uses the "default" handler.
// Return 0 if resource created successfully, otherwise -1 on error.
static int Lwm2mCore_ObjectStoreCreateOptionalResourceHandler(void * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    DefinitionRegistry * definitions = ((Lwm2mContextType *)context)->Definitions;
    ObjectStore * store = ((Lwm2mContextType *)context)->Store;

    ResourceDefinition * definition = Definition_LookupResourceDefinition(definitions, objectID, resourceID);
    if (definition == NULL)
    {
        Lwm2m_Error("No definition for object ID %d Resource ID %d\n", objectID, resourceID);
        Lwm2mResult_SetResult(Lwm2mResult_NotFound);
        goto error;
    }

    if ((resourceID = ObjectStore_CreateResource(store, objectID, objectInstanceID, resourceID)) == -1)
    {
        goto error;
    }

    if (definition->DefaultValueNode == NULL)
    {
        // load new resource with a sensible default
        Lwm2mObjectTree * objectTree = &((Lwm2mContextType *)context)->ObjectTree;
        const void * defaultData = NULL;
        int defaultLen = 0;
        if (Definition_AllocSensibleDefault(definition, &defaultData, &defaultLen) == 0)
        {
            ResourceInstanceIDType resourceInstanceID = 0;
            if (Lwm2mObjectTree_AddResourceInstance(objectTree, objectID, objectInstanceID, resourceID, resourceInstanceID) == 0)
            {
                Lwm2mCore_SetResourceInstanceValue(context, objectID, objectInstanceID, resourceID, resourceInstanceID, defaultData, defaultLen);
            }
        }
        else
        {
            Lwm2m_Error("Failed to set sensible default for /%d/%d/%d\n", objectID, objectInstanceID, resourceID);
            goto error;
        }
    }
    else
    {
        Lwm2mTreeNode * resourceInstanceNode = Lwm2mTreeNode_GetFirstChild(definition->DefaultValueNode);
        while (resourceInstanceNode)
        {
            int resourceInstanceID = 0;
            const uint8_t * resourceInstanceValue;
            uint16_t resourceInstanceValueLength;
            Lwm2mTreeNode_GetID(resourceInstanceNode, &resourceInstanceID);
            resourceInstanceValue = Lwm2mTreeNode_GetValue(resourceInstanceNode, &resourceInstanceValueLength);
            Lwm2mCore_SetResourceInstanceValue(context, objectID, objectInstanceID, resourceID, resourceInstanceID, resourceInstanceValue, resourceInstanceValueLength);
            resourceInstanceNode = Lwm2mTreeNode_GetNextChild(definition->DefaultValueNode, resourceInstanceNode);
        }
    }

    return 0;
error:
    return -1;
}

// Create an optional resource for a specified object instance.
// Return 0 on success, or -1 on error.
int Lwm2mCore_CreateOptionalResource(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if ((definition == NULL) || (definition->Handlers.CreateOptionalResource == NULL))
    {
        Lwm2m_Error("No create resource handler for object ID %d resource ID %d\n", objectID, resourceID);
        Lwm2mResult_SetResult(Lwm2mResult_NotFound);
        goto error;
    }

    if (definition->Handlers.CreateOptionalResource(context, objectID, objectInstanceID, resourceID) == 0) 
    {
        Lwm2mCore_ResourceCreated(context, objectID, objectInstanceID, resourceID);
    }
    return 0;
error:
    return -1;
}


// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode
// (at the resource level) and check it's permissions.
// Return various errors on failure, Lwm2mResult_Success on success.
Lwm2mResult Lwm2mCore_CheckWritePermissionsForResourceNode(Lwm2mContextType * context, Lwm2mRequestOrigin origin, Lwm2mTreeNode * resourceNode,
                                                           ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, bool createObjectInstance)
{
    Lwm2mResult result = Lwm2mResult_Success;
    Lwm2mTreeNode * node;
    int resourceID;
    ResourceDefinition * definition;

    Lwm2mTreeNode_GetID(resourceNode, &resourceID);
    if ((origin != Lwm2mRequestOrigin_BootstrapServer) && (origin != Lwm2mRequestOrigin_Client))
    {
        // Only allow bootstrap server and client can write to /0 as we need these for the bootstrap process
        switch (objectID)
        {
            case LWM2M_SECURITY_OBJECT:
                Lwm2m_Error("Permissions do not allow writing to %d/%d/%d\n", objectID, objectInstanceID, resourceID);
                result = Lwm2mResult_Unauthorized;
                goto error;
            default:
                break;
        }
    }

    definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if (definition == NULL)
    {
        Lwm2m_Error("Resource definition not found\n");
        result = Lwm2mResult_NotFound;
        goto error;
    }

    // Cannot create an optional resource if it already exists
    if (Lwm2mTreeNode_IsCreateFlagSet(resourceNode) && Lwm2mCore_Exists(context, objectID, objectInstanceID, resourceID))
    {
        Lwm2m_Error("Cannot create resource /%d/%d/%d: resource already exists\n", objectID, objectInstanceID, resourceID);
        result = Lwm2mResult_BadRequest;
        goto error;
    }

    // Restrict access to non-writable resources
    if (origin == Lwm2mRequestOrigin_Server && !Operations_IsResourceTypeWritable(definition->Operation) && !createObjectInstance)
    {
        Lwm2m_Error("Permissions do not allow writing to %d/%d/%d\n", objectID, objectInstanceID, resourceID);
        result = Lwm2mResult_MethodNotAllowed;
        goto error;
    }

    int numberOfNewElements = 0;

    node = Lwm2mTreeNode_GetFirstChild(resourceNode);
    while (node)
    {
        int resourceInstanceID;
        Lwm2mTreeNode_GetID(node, &resourceInstanceID);

        // If a resource is not multi-instance, than the resource ID must be 0
        if (!IS_MULTIPLE_INSTANCE(definition) && resourceInstanceID != 0)
        {
            Lwm2m_Error("Permissions do not allow for creation of multiple instances of %d/%d/%d/%d\n", objectID, objectInstanceID, resourceID, resourceInstanceID);
            result = Lwm2mResult_MethodNotAllowed;
            goto error;
        }

        if (!Lwm2mCore_Exists(context, objectID, objectInstanceID, resourceID))
        {
            numberOfNewElements++;
        }

        node = Lwm2mTreeNode_GetNextChild(resourceNode, node);
    }

    int numberOfExistingElements = Lwm2mCore_GetResourceInstanceCount(context, objectID, objectInstanceID, resourceID);
    if (numberOfNewElements + numberOfExistingElements > definition->MaximumInstances)
    {
        Lwm2m_Error("Too many resource instances for resource %d/%d/%d\n", objectID, objectInstanceID, resourceID);
        result = Lwm2mResult_MethodNotAllowed;
        goto error;
    }
error:
    return result;
}

// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode
// (at the object instance level) and check it's permissions.
// Return various errors on failure, return Lwm2mResult_Success on success.
Lwm2mResult Lwm2mCore_CheckWritePermissionsForObjectInstanceNode(Lwm2mContextType * context, Lwm2mRequestOrigin origin, Lwm2mTreeNode * objectInstanceNode, int objectID, bool createObjectInstance)
{
    Lwm2mTreeNode * node;
    int objectInstanceID;
    ObjectDefinition * definition;
    Lwm2mResult result = Lwm2mResult_Success;

    bool create = Lwm2mTreeNode_IsCreateFlagSet(objectInstanceNode) || createObjectInstance;
    bool idExists = Lwm2mTreeNode_GetID(objectInstanceNode, &objectInstanceID);

    definition = Definition_LookupObjectDefinition(context->Definitions, objectID);
    if (definition == NULL)
    {
        Lwm2m_Error("definition not found\n");
        result = Lwm2mResult_NotFound;
        goto error;
    }

    if (create && (objectInstanceID != -1) && Lwm2mCore_Exists(context, objectID, objectInstanceID, -1))
    {
        Lwm2m_Error("Cannot create object instance /%d/%d: Already exists\n", objectID, objectInstanceID);
        result = Lwm2mResult_BadRequest;
        goto error;
    }

    // If an object is not multiple instance, than the instance id must be 0,
    // or unspecified in the case we are creating a new instance.
    if (!IS_MULTIPLE_INSTANCE(definition) && (objectInstanceID != 0) && (!create || idExists))
    {
        Lwm2m_Error("Permissions do not allow for creation of multiple instances of %d\n", objectID);
        result = Lwm2mResult_MethodNotAllowed;
        goto error;
    }
    if (create && (Lwm2mCore_GetObjectNumInstances(context, objectID) + 1 > definition->MaximumInstances))
    {
        Lwm2m_Error("Cannot create object instance: object %d already contains a maximum number of instances\n", objectID);
        result = Lwm2mResult_MethodNotAllowed;
        goto error;
    }

    node = Lwm2mTreeNode_GetFirstChild(objectInstanceNode);
    while (node)
    {
        result = Lwm2mCore_CheckWritePermissionsForResourceNode(context, origin, node, objectID, objectInstanceID, createObjectInstance);
        if (result != Lwm2mResult_Success)
        {
            goto error;
        }

        node = Lwm2mTreeNode_GetNextChild(objectInstanceNode, node);
    }
error:
    return result;
}

// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode
// (at the object level) and check it's permissions.
// Return various errors on failure, return Lwm2mResult_Success on success.
Lwm2mResult Lwm2mCore_CheckWritePermissionsForObjectNode(Lwm2mContextType * context, Lwm2mRequestOrigin origin, Lwm2mTreeNode * objectNode, bool createObjectInstance)
{
    Lwm2mTreeNode * node;
    int objectID;
    Lwm2mResult result = Lwm2mResult_Success;

    Lwm2mTreeNode_GetID(objectNode, &objectID);
    bool create = Lwm2mTreeNode_IsCreateFlagSet(objectNode) || createObjectInstance;

    node = Lwm2mTreeNode_GetFirstChild(objectNode);
    while (node)
    {
        result = Lwm2mCore_CheckWritePermissionsForObjectInstanceNode(context, origin, node, objectID, create);
        if (result != Lwm2mResult_Success)
        {
            return result;
        }
        node = Lwm2mTreeNode_GetNextChild(objectNode, node);
    }
    return result;
}

// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode (at the resource level)
// and write any values by making calls to Lwm2mCore_SetResourceInstanceValue(). Write permissions must already be checked.
// Return various errors on failure, return Lwm2mResult_Success on success.
static Lwm2mResult Lwm2mCore_ParseResourceNodeAndWriteToStore(Lwm2mContextType * context, Lwm2mTreeNode * resourceNode, ObjectIDType objectID,
                                                              ObjectInstanceIDType objectInstanceID, bool createOptionalResource)
{
    int resourceID;
    Lwm2mResult result = Lwm2mResult_SuccessChanged;

    Lwm2mTreeNode_GetID(resourceNode, &resourceID);

    if (createOptionalResource || Lwm2mTreeNode_IsCreateFlagSet(resourceNode))
    {
        ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
        if (!IS_MANDATORY(definition) && !Lwm2mCore_Exists(context, objectID, objectInstanceID, resourceID))
        {
            if (Lwm2mCore_CreateOptionalResource(context, objectID, objectInstanceID, resourceID) == -1)
            {
                Lwm2m_Error("Failed to create optional resource: /%d/%d/%d\n", objectID, objectInstanceID, resourceID);
                result = Lwm2mResult_InternalError;
                goto error;
            }
        }
    }

    // Perform the write
    Lwm2mTreeNode * resourceInstanceNode = Lwm2mTreeNode_GetFirstChild(resourceNode);
    while (resourceInstanceNode)
    {
        uint16_t length;
        const uint8_t * value;
        int id;

        Lwm2mTreeNode_GetID(resourceInstanceNode, &id);

        value = Lwm2mTreeNode_GetValue(resourceInstanceNode, &length);
        if (value && length > 0)
        {
            if (Lwm2mCore_SetResourceInstanceValue(context, objectID, objectInstanceID, resourceID, id, value, length) != 0)
            {
                result = Lwm2mResult_InternalError;
                break;
            }
        }
        resourceInstanceNode = Lwm2mTreeNode_GetNextChild(resourceNode, resourceInstanceNode);
    }
error:
    return result;
}

// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode (at the object instance level)
// and write any values by making calls to Lwm2mCore_SetResourceInstanceValue(). Write permissions must already be checked.
// Return various errors on failure, return Lwm2mResult_Success on success.
static Lwm2mResult Lwm2mCore_ParseObjectInstanceNodeAndWriteToStore(Lwm2mContextType * context, Lwm2mTreeNode * instanceNode, ObjectIDType objectID,
                                                                    bool createObjectInstance, bool createOptionalResources, bool replace, int * newObjectInstanceID)
{
    Lwm2mResult result = Lwm2mResult_SuccessChanged;

    Lwm2mTreeNode_GetID(instanceNode, newObjectInstanceID);

    if (replace)
    {
        // Special handling for a bootstrap write
        if ((*newObjectInstanceID != -1) && Lwm2mCore_Exists(context, objectID, *newObjectInstanceID, -1))
        {
            result = Lwm2mResult_SuccessChanged;
        }
        else if ((*newObjectInstanceID = Lwm2mCore_CreateObjectInstance(context, objectID, *newObjectInstanceID)) != -1)
        {
             result = Lwm2mResult_SuccessChanged;
        }
        else
        {
            result = Lwm2mResult_BadRequest;
            goto error;
        }
    }
    else if (createObjectInstance || Lwm2mTreeNode_IsCreateFlagSet(instanceNode))
    {
        if ((*newObjectInstanceID = Lwm2mCore_CreateObjectInstance(context, objectID, *newObjectInstanceID)) == -1)
        {
            Lwm2m_Error("Failed to create object instance\n");
            result = Lwm2mResult_BadRequest;
            goto error;
        }
        else
        {
            result = Lwm2mResult_SuccessCreated;
        }
    }

    if (*newObjectInstanceID == -1)
    {
        Lwm2m_Error("Object instance ID is -1\n");
        goto error;
    }

    Lwm2mTreeNode * resourceNode = Lwm2mTreeNode_GetFirstChild(instanceNode);
    while (resourceNode != NULL)
    {
        Lwm2mResult writeResourceResult = Lwm2mCore_ParseResourceNodeAndWriteToStore(context, resourceNode, objectID, *newObjectInstanceID, createOptionalResources);
        if (!Lwm2mResult_IsSuccess(writeResourceResult))
        {
            result = writeResourceResult;
            break;
        }
        resourceNode = Lwm2mTreeNode_GetNextChild(instanceNode, resourceNode);
    }
error:
    return result;
}

// This function is called when an LWM2M write operation is performed, it is used to walk a Lwm2mTreeNode (at the object level)
// and write any values by making calls to Lwm2mCore_SetResourceInstanceValue(). Write permissions must already be checked.
// Return various errors on failure, return Lwm2mResult_Success on success.
Lwm2mResult Lwm2mCore_ParseObjectNodeAndWriteToStore(Lwm2mContextType * context, Lwm2mTreeNode * objectNode, bool createObjectInstance, 
                                                     bool createOptionalResources, bool replace, int * newObjectInstanceID)
{
    Lwm2mTreeNode * node;
    int objectID;
    Lwm2mResult result = Lwm2mResult_Success;

    Lwm2mTreeNode_GetID(objectNode, &objectID);
    node = Lwm2mTreeNode_GetFirstChild(objectNode);

    while (node != NULL)
    {
        result = Lwm2mCore_ParseObjectInstanceNodeAndWriteToStore(context, node, objectID, createObjectInstance, createOptionalResources, replace, newObjectInstanceID);
        if (!Lwm2mResult_IsSuccess(result))
        {
            break;
        }
        node = Lwm2mTreeNode_GetNextChild(objectNode, node);
    }
    return result;
}

/**
 * @brief Register a new object type definition.
 * @param[in] context
 * @param[in] objName descriptive name of object
 * @param[in] objectID object type ID to register
 * @param[in] multiInstance is this object multiple or single instance
 * @param[in] mandatory is this object mandatory?
 * @param[in] handlers pointer to ObjectOperationsHandlers, use &defaultObjectOperationHandlers if custom behaviour is not required
 * @return -1 on error 0 on success
 */
int Lwm2mCore_RegisterObjectType(Lwm2mContextType * context, const char * objName, ObjectIDType objectID, uint16_t maximumInstances,
                                 uint16_t minimumInstances, ObjectOperationHandlers * handlers)
{
    int result;

    if (Definition_LookupObjectDefinition(context->Definitions, objectID) == NULL)
    {
        Lwm2mCore_ObjectCreated(context, objectID);
    }

    if ((result = Definition_RegisterObjectType(context->Definitions, objName, objectID, maximumInstances, minimumInstances, handlers)) != 0)
    {
        goto error;
    }

    return result;
error:
    return -1;
}

/**
 * @brief Register a new resource type definition.
 * @param[in] context
 * @param[in] resName descriptive name of resource
 * @param[in] objectID object type ID to register resource against. i.e 1000 in /1000/_/_
 * @param[in] resourceID resource type ID to register i.e 101 in /_/_/101
 * @param[in] resourceType data type for this resource. Integer, String etc.
 * @param[in] multiInstance is this resource multiple or single instance
 * @param[in] mandatory is this resource mandatory?
 * @param[in] operations allowed operations, read, write, execute etc.
 * @param[in] handlers pointer to ResourceOperationsHandlers, use &defaultResourceOperationHandlers if custom behaviour is not required
 * @return -1 on error 0 on success
 */
int Lwm2mCore_RegisterResourceType(Lwm2mContextType * context, const char * resName, ObjectIDType objectID, ResourceIDType resourceID, ResourceTypeType resourceType,
                                   uint16_t maximumInstances, uint16_t minimumInstances, Operations operations, ResourceOperationHandlers * handlers)
{
    return Definition_RegisterResourceType(context->Definitions, resName, objectID, resourceID, resourceType, maximumInstances, minimumInstances, operations, handlers, NULL);
}

int Lwm2mCore_RegisterResourceTypeWithDefaultValue(Lwm2mContextType * context, const char * resName, ObjectIDType objectID, ResourceIDType resourceID, ResourceTypeType resourceType,
                                   uint16_t maximumInstances, uint16_t minimumInstances, Operations operations, ResourceOperationHandlers * handlers, Lwm2mTreeNode * defaultValueNode)
{
    return Definition_RegisterResourceType(context->Definitions, resName, objectID, resourceID, resourceType, maximumInstances, minimumInstances, operations, handlers, defaultValueNode);
}

/**
 * @brief Delete a single, or all instances of an object and any associated resources.
 * @param[in] context
 * @param[in] objectID
 * @param[in] objectInstanceID
 * @return Lwm2mResult_SuccessDeleted on success
 * @return various errors on failure
 */
Lwm2mResult Lwm2mCore_Delete(Lwm2mContextType * context, Lwm2mRequestOrigin requestOrigin, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    // According to the standard a DELETE must be O/I, not O/I/R,
    // only the client or bootstrap server has the authorisation to delete individual resources.
    if ((resourceID != -1) && (requestOrigin == Lwm2mRequestOrigin_Server))
    {
        return Lwm2mResult_MethodNotAllowed;
    }

    if (objectID == -1)
    {
        if (requestOrigin != Lwm2mRequestOrigin_BootstrapServer)
        {
            return Lwm2mResult_Unauthorized;
        }
        else
        {
            // Delete on '/' - Call delete on everything.
            ObjectIDType objectID = -1;
            while ((objectID = Lwm2mCore_GetNextObjectID(context, objectID)) != -1)
            {
                // Best effort attempt
                Lwm2mCore_Delete(context, requestOrigin, objectID, -1, -1);
            }
            return Lwm2mResult_SuccessDeleted;
        }
    }
    else if (objectInstanceID == -1)
    {
        if (requestOrigin == Lwm2mRequestOrigin_Server)
        {
            // ObjectInstance is required for CoAP delete requests.
            return Lwm2mResult_MethodNotAllowed;
        }
    }

    ObjectDefinition * definition = Definition_LookupObjectDefinition(context->Definitions, objectID);
    if ((definition == NULL) || (definition->Handlers.Delete == NULL))
    {
        Lwm2m_Error("No delete handler for object ID %d\n", objectID);
        return Lwm2mResult_NotFound;
    }

    if ((requestOrigin == Lwm2mRequestOrigin_Server) && !IS_MULTIPLE_INSTANCE(definition) && IS_MANDATORY(definition) && (objectInstanceID == 0))
    {
        // If the object is marked mandatory and single-instance, we must have at least one instance.
        return Lwm2mResult_Unauthorized;
    }

    if (requestOrigin == Lwm2mRequestOrigin_Server)
    {
        // Only allow bootstrap server to write to /0 or /1 as we need these for the bootstrap process.
        switch (objectID)
        {
            case LWM2M_SECURITY_OBJECT: // no break
            case LWM2M_SERVER_OBJECT:
                return Lwm2mResult_Unauthorized;
            default:
                break;
        }
    }

    int ret = definition->Handlers.Delete(context, objectID, objectInstanceID, resourceID);
    if (ret != -1)
    {
        char path[32];
        if (resourceID != -1)
        {
            Lwm2mObjectTree_DeleteResource(&context->ObjectTree, objectID, objectInstanceID, resourceID);
            sprintf(path, "/%d/%d/%d", objectID, objectInstanceID, resourceID);
        }
        else if (objectInstanceID != -1)
        {
            Lwm2mObjectTree_DeleteObjectInstance(&context->ObjectTree, objectID, objectInstanceID);
            sprintf(path, "/%d/%d", objectID, objectInstanceID);

            Lwm2m_SetUpdateRegistration(context);
        }
        else
        {
            // Special case - shouldn't be able to delete the entire security object
            if (objectID != 0)
            {
                Lwm2mObjectTree_DeleteObject(&context->ObjectTree, objectID);
                sprintf(path, "/%d", objectID);
                Lwm2m_SetUpdateRegistration(context);
            }
        }

        // Special case - shouldn't be able to delete the entire security object
        if ((objectID != 0) || (objectInstanceID != -1))
        {
            if (Lwm2mCore_FindResourceEndPoint(&context->EndPointList, path) != NULL)
            {
                ret = Lwm2mCore_RemoveResourceEndPoint(&context->EndPointList, path);
            }
        }

        Lwm2m_RemoveAllObserversForOIR(context, objectID, objectInstanceID, resourceID);

        // The LWM2M specification does not specify how a client should notify a server
        // that an observed resource has been removed.
    }

    return (ret == 0) ? Lwm2mResult_SuccessDeleted : Lwm2mResult_NotFound;
}

// Check to see if an Object, ObjectInstance or Resource exists. Return 1 if exists, or 0 if the specified entity does not exist.
int Lwm2mCore_Exists(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    return Lwm2mObjectTree_Exists(&context->ObjectTree, objectID, objectInstanceID, resourceID, -1);
}

// Register an Observer of a specific resource. Return -1 on error, 0 on success.
int Lwm2mCore_Observe(Lwm2mContextType * context, AddressType * addr, const char * token, int tokenLength, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID,
                      ResourceIDType resourceID, ContentType contentType, Lwm2mNotificationCallback callback, void * contextData)
{
    return Lwm2m_Observe(context, addr, token, tokenLength, objectID, objectInstanceID, resourceID, contentType, callback, contextData);
}

// Deregister an Observer from a specific resource. Return -1 on error, 0 on success.
int Lwm2mCore_CancelObserve(Lwm2mContextType * context, AddressType * addr, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    // LWM2M specifies that the observation token ID should be used to cancel an observation. Instead, cancel based on the resource ID.
    return Lwm2m_CancelObserve(context, addr, objectID, objectInstanceID, resourceID);
}

// Set the value of a resource instance. Return -1 on error, 0 or greater on success.
int Lwm2mCore_SetResourceInstanceValue(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID,
                                       ResourceInstanceIDType resourceInstanceID, const void * value, int valueSize)
{
    bool changed = false;

    ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if ((definition == NULL) || (definition->Handlers.Write == NULL))
    {
        return -1;
    }

    if (definition->Handlers.Write(context, objectID, objectInstanceID, resourceID, resourceInstanceID, (uint8_t*)value, valueSize, &changed) >= 0)
    {
        Lwm2mObjectTree_AddResourceInstance(&context->ObjectTree, objectID, objectInstanceID, resourceID, resourceInstanceID);
        if (changed)
        {
            Lwm2m_MarkObserversChanged(context, objectID, objectInstanceID, resourceID, value, valueSize);
        }
        return 0;
    }
    return -1;
}

// Execute a resource and pass in the provided value. Return -1 on error, 0 or greater on success.
int Lwm2mCore_ResourceExecute(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID, const void * value, int valueSize)
{
    ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if ((definition == NULL) || (definition->Handlers.Execute == NULL))
    {
        return -1;
    }
    return definition->Handlers.Execute(context, objectID, objectInstanceID, resourceID, (uint8_t*)value, valueSize);
}

int Lwm2mCore_GetResourceInstanceLength(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID)
{
    ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if ((definition == NULL) || (definition->Handlers.GetLength == NULL))
    {
        return -1;
    }
    return definition->Handlers.GetLength(context, objectID, objectInstanceID, resourceID, resourceInstanceID);
}

int Lwm2mCore_GetResourceInstanceCount(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    int valueID = -1;
    int count = 0;

    while ((valueID = Lwm2mCore_GetNextResourceInstanceID(context, objectID, objectInstanceID, resourceID, valueID)) != -1)
    {
        count++;
    }
    return count;
}

ObjectIDType Lwm2mCore_GetNextObjectID(Lwm2mContextType * context, ObjectIDType objectID)
{
    Lwm2mObjectTreeIterator iterator;
    Lwm2mObjectTree_InitIterator(&iterator, &context->ObjectTree, objectID, -1, -1, -1);
    return Lwm2mObjectTree_GetNextObjectID(&context->ObjectTree, &iterator);
}

ObjectInstanceIDType Lwm2mCore_GetNextObjectInstanceID(Lwm2mContextType * context, ObjectIDType  objectID, ObjectInstanceIDType objectInstanceID)
{
    Lwm2mObjectTreeIterator iterator;
    Lwm2mObjectTree_InitIterator(&iterator, &context->ObjectTree, objectID, objectInstanceID, -1, -1);
    return Lwm2mObjectTree_GetNextObjectInstanceID(&context->ObjectTree, &iterator);
}

int Lwm2mCore_GetObjectNumInstances(Lwm2mContextType * context, ObjectIDType objectID)
{
    return Lwm2mObjectTree_GetNumObjectInstances(&context->ObjectTree, objectID);
}

ResourceIDType Lwm2mCore_GetNextResourceID(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID)
{
    Lwm2mObjectTreeIterator iterator;
    Lwm2mObjectTree_InitIterator(&iterator, &context->ObjectTree, objectID, objectInstanceID, resourceID, -1);
    return Lwm2mObjectTree_GetNextResourceID(&context->ObjectTree, &iterator);
}

ResourceInstanceIDType Lwm2mCore_GetNextResourceInstanceID(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID)
{
    Lwm2mObjectTreeIterator iterator;
    Lwm2mObjectTree_InitIterator(&iterator, &context->ObjectTree, objectID, objectInstanceID, resourceID, resourceInstanceID);
    return Lwm2mObjectTree_GetNextResourceInstanceID(&context->ObjectTree, &iterator);
}

int Lwm2mCore_GetResourceInstanceValue(Lwm2mContextType * context, ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID, ResourceInstanceIDType resourceInstanceID, void * Value, int ValueBufferSize)
{
    ResourceDefinition * definition = Definition_LookupResourceDefinition(context->Definitions, objectID, resourceID);
    if ((definition == NULL) || (definition->Handlers.Read == NULL))
    {
        return -1;
    }
    return definition->Handlers.Read(context, objectID, objectInstanceID, resourceID, resourceInstanceID, (uint8_t *)Value, ValueBufferSize);
}

/**
 * @brief Iterate through all instances of an Object in the object store and construct
 *        a list in the CoRE link format.
 * @param store pointer to object store
 * @param altPath By default, the RFC6690 links of Objects are located under the root path
 *                However, devices might be hosting other Resources on an endpoint, and there may be the need to place
 *                Objects under an alternative path.
 * @param buffer
 * @param len
 * @return pointer to null terminated string (must be freed)
 */
static int Lwm2mCore_GetObjectListForObjectID(Lwm2mContextType * context, ObjectIDType objectID, char * altPath, char * buffer, int len, bool * first)
{
    int pos = 0;
    int numInstances = Lwm2mCore_GetObjectNumInstances(context, objectID);

    if (!numInstances)
    {
        // object type is supported but no instances exist
        pos += snprintf(buffer + pos, len - pos, "%s<%s%d>", *first ? "" : ",", altPath ? altPath : "/", objectID);
        *first = false;
    }

    ObjectInstanceIDType objectInstanceID = -1;
    while ((objectInstanceID = Lwm2mCore_GetNextObjectInstanceID(context, objectID, objectInstanceID)) != -1)
    {
        pos += snprintf(buffer + pos, len - pos, "%s<%s%d/%d>", *first ? "" : ",", altPath ? altPath : "/", objectID, objectInstanceID);
        *first = false;
    }

    return pos;
}

/**
 * @brief Iterate through all of the objects in the object store and construct
 *        a list in the CoRE link format.
 * @param store pointer to object store
 * @param altPath By default, the RFC6690 links of Objects are located under the root path
 *                However, devices might be hosting other Resources on an endpoint, and there may be the need to place
 *                Objects under an alternative path.
 * @param buffer
 * @param len
 * @return pointer to null terminated string (must be freed)
 */
void Lwm2mCore_GetObjectList(Lwm2mContextType * context, char * altPath, char * buffer, int len, bool updated)
{
    memset(buffer, 0, len);

    if (updated)
    {
        int objectID = -1;
        int pos = 0;

#ifdef CONTIKI
        bool first = true;
#else
        // Always add "ct" field for </> to signal we support JSON.
        pos += snprintf(buffer + pos, len - pos, "<%s>;ct=%d", altPath ? altPath : "/", ContentType_ApplicationOmaLwm2mJson);
        bool first = false;
#endif // CONTIKI

        // Loop through all objects in the object store.
        while ((objectID = Definition_GetNextObjectType(context->Definitions, objectID)) != -1)
        {
            if (objectID != LWM2M_SECURITY_OBJECT)
            {
                pos += Lwm2mCore_GetObjectListForObjectID(context, objectID, altPath, buffer + pos, len, &first);
            }
        }
    }
}

// Handler LwM2M Notifications and send them as CoAP messages. Return 0 on success, non-zero on error.
static int Lwm2m_HandleNotification(void * ctxt, AddressType * addr, int sequence, const char * token, int tokenLength,
                                    ObjectIDType objectID, ObjectInstanceIDType objectInstanceID, ResourceIDType resourceID, ContentType contentType, void * ContextData)
{
    Lwm2mContextType * context = (Lwm2mContextType*)ctxt;
    int oir[3];
    ObjectInstanceResourceKey key = { objectID, objectInstanceID, resourceID };
    char path[128] = {0};
    int matches;
    ContentType payloadContentType;
    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    Lwm2mCore_AddressTypeToPath(path, addr);
    strcat(path, "/");
    strcat(path, OirToUri(key));

    matches = sscanf(OirToUri(key), "%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);

    Lwm2mTreeNode * dest;
    if (TreeBuilder_CreateTreeFromOIR(&dest, context, origin, oir, matches) == Lwm2mResult_Success)
    {
        char payload[1024];
        int payloadLen = Lwm2mCore_SerialiseOIR(dest, contentType, oir, matches, &payloadContentType, payload, sizeof(payload));
        Lwm2mTreeNode_DeleteRecursive(dest);

        if (payloadLen >= 0)
        {
            Lwm2m_Debug("Send Notify to %s\n", path);
            coap_SendNotify(addr, path, token, tokenLength, payloadContentType, payload, payloadLen, sequence);
        }
    }
    return 0;
}

// Handle CoAP GET Requests with Observe, Maps to LWM2M Observe. Return 0 on success, non-zero on error.
static int Lwm2mCore_HandleObserveRequest(void * ctxt, AddressType * addr, const char * path, const char * query, char * token, int tokenLength,
                                          ContentType contentType, const char * requestContent, int requestContentLen, ContentType * responseContentType,
                                          char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int matches;
    int oir[3] = { -1, -1, -1 };

    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    *responseContentType = ContentType_None;
    Lwm2mResult result = Lwm2mResult_Unspecified;

    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);

    if (contentType == ContentType_ApplicationLinkFormat)
    {
        // Do not support an Observe and Discover in the same request.
        *responseContentLen = 0;
        *responseCode = Lwm2mResult_MethodNotAllowed;
    }
    else if (Lwm2mCore_Exists(context, oir[0], oir[1], oir[2]))
    {
        int len = 0;
        if (Lwm2mCore_Observe(context, addr, token, tokenLength, oir[0], oir[1], oir[2], contentType, Lwm2m_HandleNotification, NULL) != -1)
        {
            Lwm2mTreeNode * root;
            if ((result = TreeBuilder_CreateTreeFromOIR(&root, context, origin, oir, matches)) == Lwm2mResult_Success)
            {
                len = Lwm2mCore_SerialiseOIR(root, contentType, oir, matches, responseContentType, responseContent, *responseContentLen);
            }
            Lwm2mTreeNode_DeleteRecursive(root);
        }

        *responseContentLen = (len >= 0) ? len : 0;
        *responseCode = (len >= 0) ? Lwm2mResult_SuccessContent : result;
    }
    else
    {
        *responseContentLen = 0;
        *responseCode = Lwm2mResult_NotFound;
    }
    return 0;
}

// Handle CoAP GET Requests with Cancel Observe, Maps to LWM2M CancelObserve. Return 0 on success, non-zero on error.
static int Lwm2mCore_HandleCancelObserveRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                                const char * requestContent, int requestContentLen, ContentType * responseContentType,
                                                char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int matches;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    Lwm2mResult result = Lwm2mResult_Unspecified;
    *responseContentType = ContentType_None;
    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);
    if (matches > 0)
    {
        int len = -1;
        Lwm2mCore_CancelObserve(context, addr, oir[0], oir[1], oir[2]);

        // Perform "GET", to return clientID in content.
        Lwm2mTreeNode * root;
        if ((result = TreeBuilder_CreateTreeFromOIR(&root, context, origin, oir, matches)) == Lwm2mResult_Success)
        {
            len = Lwm2mCore_SerialiseOIR(root, contentType, oir, matches, responseContentType, responseContent, *responseContentLen);
        }
        Lwm2mTreeNode_DeleteRecursive(root);

        if (len >= 0)
        {
            *responseContentLen = len;
            *responseCode = Lwm2mResult_SuccessContent;
        }
        else
        {
            *responseContentLen = 0;
            *responseCode = result;
        }
    }
    else
    {
        *responseContentLen = 0;
        *responseCode = Lwm2mResult_NotFound;
    }
    return 0;
}

// Handler CoAP GET Requests, maps onto LWM2M READ and DISCOVER operations. Return 0 on success, non-zero on error.
static int Lwm2mCore_HandleGetRequest(void * ctxt, AddressType * addr, const char * path, const char * query,
                                      ContentType acceptContentType, const char * requestContent, int requestContentLen,
                                      ContentType * responseContentType, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int len = 0;
    int matches;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    *responseContentType = ContentType_None;
    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);
    Lwm2mResult result = Lwm2mResult_Unspecified;

    if (acceptContentType == ContentType_ApplicationLinkFormat)
    {
        Lwm2m_Debug("Discover not supported\n");
    }
    else
    {
        Lwm2m_Debug("Read\n");
        Lwm2mTreeNode * root;
        if ((result = TreeBuilder_CreateTreeFromOIR(&root, context, origin, oir, matches)) == Lwm2mResult_Success)
        {
            len = Lwm2mCore_SerialiseOIR(root, acceptContentType, oir, matches, responseContentType, responseContent, *responseContentLen);
        }
        else
        {
            // Not found or some other failure.
            len = -1;
        }
        Lwm2mTreeNode_DeleteRecursive(root);
    }

    *responseContentLen = (len < 0) ? 0 : len;
    *responseCode = (len >= 0) ? Lwm2mResult_SuccessContent : result;
    return 0;
}

// Handler CoAP POST Requests, maps onto LWM2M CREATE and Partial Update (WRITE) operations.
// Partial Update: adds or updates Resources or Resource Instances provided in the new value
// and leaves other existing Resources or Resource Instances unchanged.
// Return 0 on success, non-zero on error.
static int Lwm2mCore_HandlePostRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                       const char * requestContent, int requestContentLen, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int len = -1;
    int matches;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    *responseContentLen = 0;

    // 3 types of POST to deal with:
    //  1.  Write: Partial Update (LWM2M 5.4.3):
    //        /O/I - update existing Resources or Resources instances in Object Instance, existing Resources and Resource instances are unchanged
    //        /O/I - create/add new Resource and Resource Instances specified in payload for Object Instance, existing Resources and Resource instances are unchanged
    //        /O/I/R - can update existing Resource Instances
    //        /O/I/R - can update existing Resources
    //  2.  Execute (LWM2M 5.4.5):
    //        /O/I/R + internal "execute" attribute on R
    //  3.  Create (LWM2M 5.4.6):
    //        /O + instance in payload

    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);

    if (Definition_IsResourceTypeExecutable(context->Definitions, oir[0], oir[2]) == 1)
    {
        // Handle EXECUTE
        Lwm2m_Debug("EXECUTE: %s\n", path);
        len = Lwm2mCore_ResourceExecute(context, oir[0], oir[1], oir[2], 0, requestContent, requestContentLen);

        *responseContentLen = 0;
        if (len >= 0)
        {
            *responseCode = Lwm2mResult_SuccessChanged;
        }
        else
        {
            // execute not allowed
            *responseCode = Lwm2mResult_MethodNotAllowed;
        }
    }
    else
    {
        // Handle WRITE and CREATE
        Lwm2mTreeNode * root = NULL;
        len = Lwm2mCore_DeserialiseOIR(&root, contentType, context, oir, matches, requestContent, requestContentLen);
        
        if (len >= 0)
        {
            bool createObjectInstance = false;
            if (matches == 1)
            {
                // Create
                if (Lwm2mTreeNode_GetType(Lwm2mTreeNode_GetFirstChild(root)) == Lwm2mTreeNodeType_ObjectInstance)
                {
                    Lwm2mTreeNode * instance = Lwm2mTreeNode_GetFirstChild(root);
                    Lwm2mTreeNode_GetID(instance, &oir[1]);
                    Lwm2m_Debug("CREATE (Instance ID in payload): /%d/%d\n", oir[0], oir[1]);
                }
                else
                {
                    Lwm2m_Debug("CREATE (no Instance ID in payload): /%d\n", oir[0]);

                    // This is an object node with resource values but no object instance.
                    // Treat it as an object instance node and add it to an object node.
                    Lwm2mTreeNode * object = Lwm2mTreeNode_Create();
                    Lwm2mTreeNode_SetID(object, oir[0]);
                    Lwm2mTreeNode_SetType(object, Lwm2mTreeNodeType_Object);
                    Lwm2mTreeNode_AddChild(object, root);

                    Lwm2mTreeNode_SetID(root, -1);
                    Lwm2mTreeNode_SetType(root, Lwm2mTreeNodeType_ObjectInstance);
                    root = object;
                }
                createObjectInstance = true;
            }
            else
            {
                Lwm2m_Debug("WRITE (partial update): %s\n", path);
            }
            
            switch (Lwm2mTreeNode_GetType(root))
            {
                case Lwm2mTreeNodeType_Object:
                    if ((*responseCode = Lwm2mCore_CheckWritePermissionsForObjectNode(context, origin, root, createObjectInstance)) == Lwm2mResult_Success)
                    {
                        *responseCode = Lwm2mCore_ParseObjectNodeAndWriteToStore(context, root, true, true, false, &oir[1]);
                    }
                    break;
                case Lwm2mTreeNodeType_ObjectInstance:
                    if ((*responseCode = Lwm2mCore_CheckWritePermissionsForObjectInstanceNode(context, origin, root, oir[0], createObjectInstance)) == Lwm2mResult_Success)
                    {
                        *responseCode = Lwm2mCore_ParseObjectInstanceNodeAndWriteToStore(context, root, oir[0], false, true, false, &oir[1]);
                    }
                    break;
                case Lwm2mTreeNodeType_Resource:
                    if ((*responseCode = Lwm2mCore_CheckWritePermissionsForResourceNode(context, origin, root, oir[0], oir[1], createObjectInstance)) == Lwm2mResult_Success)
                    {
                        *responseCode = Lwm2mCore_ParseResourceNodeAndWriteToStore(context, root, oir[0], oir[1], true);
                    }
                    break;
                case Lwm2mTreeNodeType_ResourceInstance: // no break
                default:
                    // Should never happen..
                    *responseCode = Lwm2mResult_BadRequest;
                    break;
            }

            if (createObjectInstance && Lwm2mResult_IsSuccess(*responseCode))
            {
                // Copy location into response to convey it to the CoAP abstraction layer.
                *responseContentLen = sprintf(responseContent, "/%d/%d", oir[0], oir[1]);
            }
        }
        else
        {
            Lwm2m_Error("Failed to deserialise content type %d, len %d\n", contentType, len);
            *responseCode = Lwm2mResult_BadRequest;
        }
        Lwm2mTreeNode_DeleteRecursive(root);
    }
    return 0;
}

// PUT Write Attributes request
//   2.04 Changed “Write Attributes” operation is completed successfully
//   4.00 Bad Request The format of attribute to be written is different
//   4.04 Not Found URI of “Write Attributes” operation is not found
//   4.01 Unauthorized Access Right Permission Denied
//   4.05 Method Not Allowed Target is not allowed for Write Attributes operation
static int Lwm2mCore_HandleWriteAttributesRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                                  const char * requestContent, int requestContentLen, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int oir[3] = { -1, -1, -1 };
    int matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);

    Lwm2m_Debug("WriteAttributes request: %s\n", query);
    *responseContentLen = 0;

    if ((matches == 3) && (Definition_IsResourceTypeExecutable(context->Definitions, oir[0], oir[2]) == 1))
    {
        Lwm2m_Error("Write attributes cannot be set on an executable resource: %s\n", path);
        *responseCode = Lwm2mResult_MethodNotAllowed;
        goto error;
    }

    NotificationAttributes * attributes = AttributeStore_LookupNotificationAttributes(context->AttributeStore, Lwm2mSecurity_GetShortServerID(context, addr), oir[0], oir[1], oir[2]);
    if (attributes != NULL)
    {
        // Store changes in a temporary object and only write them on success.
        NotificationAttributes temp;
        memcpy(&temp, attributes, sizeof(NotificationAttributes));

        int numPairs = 0;
        QueryPair * pairs = Lwm2mCore_SplitQuery(query, &numPairs);

        if (pairs != NULL)
        {
            int i = 0;
            for (i = 0; i < numPairs; i++)
            {
                QueryPair * pair = &pairs[i];
                Lwm2m_Debug("Pair %d: %s = %s\n", i, pair->Key, pair->Value);

                const AttributeCharacteristics * characteristics = Lwm2mAttributes_GetAttributeCharacteristics(pair->Key);
                if (characteristics != NULL)
                {
                    Lwm2m_Debug("Found write attribute characteristics %s - checking value type\n", characteristics->Name);

                    int integerValue = 0;
                    float floatValue = 0;

                    switch (characteristics->ValueType)
                    {
                        case ResourceTypeEnum_TypeInteger:
                        {
                            if ((pair->Value != NULL) && (sscanf(pair->Value, "%24d", &integerValue) == 0))
                            {
                                Lwm2m_Error("Failed to parse integer value %s for write attribute: %s\n", pair->Value, pair->Key);
                                *responseCode = Lwm2mResult_BadRequest;
                                goto error;
                            }
                            break;
                        }
                        case ResourceTypeEnum_TypeFloat:
                        {
                            if ((pair->Value != NULL) && (sscanf(pair->Value, "%24f", &floatValue) == 0))
                            {
                                Lwm2m_Error("Failed to parse float value %s for write attribute: %s\n", pair->Value, pair->Key);
                                *responseCode = Lwm2mResult_BadRequest;
                                goto error;
                            }
                            break;
                        }
                        case ResourceTypeEnum_TypeNone:
                            break;
                        default:
                            Lwm2m_Error("Unsupported resource type for write attribute: %d\n", characteristics->ValueType);
                            *responseCode = Lwm2mResult_InternalError;
                            goto error;
                            break;
                    }

                    if (pair->Value != NULL)
                    {
                        switch (characteristics->Type)
                        {
                            case AttributeTypeEnum_Cancel:
                                Lwm2mCore_CancelObserve(context, addr, oir[0], oir[1], oir[2]);
                                break;
                            case AttributeTypeEnum_MinimumPeriod:
                                temp.MinimumPeriod = integerValue;
                                Lwm2m_Debug("Set minimum period to: %d\n", integerValue);
                                break;
                            case AttributeTypeEnum_MaximumPeriod:
                                temp.MaximumPeriod = integerValue;
                                Lwm2m_Debug("Set maximum period to: %d\n", integerValue);
                                break;
                            case AttributeTypeEnum_GreaterThan:
                                temp.GreaterThan = floatValue;
                                Lwm2m_Debug("Set greaterthan to: %f\n", floatValue);
                                break;
                            case AttributeTypeEnum_LessThan:
                                temp.LessThan = floatValue;
                                Lwm2m_Debug("Set lessthan to: %f\n", floatValue);
                                break;
                            case AttributeTypeEnum_Step:
                                temp.Step = floatValue;
                                Lwm2m_Debug("Set step to: %f\n", floatValue);
                                break;
                            default:
                                Lwm2m_Error("Unsupported resource type for write attribute: %d\n", characteristics->ValueType);
                                *responseCode = Lwm2mResult_InternalError;
                                goto error;
                                break;
                        }
                    }
                    temp.Valid[characteristics->Type] = pair->Value != NULL;
                    Lwm2m_Debug("Attribute %s set valid: %d\n", characteristics->Name, temp.Valid[characteristics->Type]);
                }
                else
                {
                    Lwm2m_Error("No write attribute matches query key: %s\n", pair->Key);
                    *responseCode = Lwm2mResult_BadRequest;
                    goto error;
                }
            }

            int shortServerID = Lwm2mSecurity_GetShortServerID(context, addr);
            int minimumPeriod = temp.Valid[AttributeTypeEnum_MinimumPeriod] ? temp.MinimumPeriod : Lwm2mServerObject_GetDefaultMinimumPeriod(context, shortServerID);
            int maximumPeriod = temp.Valid[AttributeTypeEnum_MaximumPeriod] ? temp.MaximumPeriod : Lwm2mServerObject_GetDefaultMaximumPeriod(context, shortServerID);

            if ((maximumPeriod != -1) && (minimumPeriod > maximumPeriod))
            {
                Lwm2m_Error("Attempt to set maximum period to less than minimum period\n");
                *responseCode = Lwm2mResult_BadRequest;
            }
            else if (temp.Valid[AttributeTypeEnum_GreaterThan] && temp.Valid[AttributeTypeEnum_LessThan] && temp.Valid[AttributeTypeEnum_Step])
            {
                // The following rules MUST be respected (“lt” value + 2*”stp” values < “gt” value)
                Lwm2m_Error("The difference between minimum and maximum threshold is less than twice the step attribute value\n");
                *responseCode = Lwm2mResult_BadRequest;
            }
            else
            {
                // Query was fully checked - copy attributes
                memcpy(attributes, &temp, sizeof(NotificationAttributes));
                *responseCode = Lwm2mResult_SuccessChanged;
            }
            Lwm2mCore_FreeQueryPairs(pairs, numPairs);
            pairs = NULL;
        }
        else
        {
            *responseCode = Lwm2mResult_BadRequest;
        }
    }
    else
    {
        *responseCode = Lwm2mResult_NotFound;
    }

error:
    return ContentType_None;
}

// Handle CoAP PUT Requests, maps onto LWM2M Replace WRITE and WRITE ATTRIBUTES operations.
// LWM2M Spec 5.4.3: Replace: replaces the Object Instance or the Resource(s) with the new value provided in the “Write” operation.
// Return 0 on success, non-zero on error.
static int Lwm2mCore_HandlePutRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                      const char * requestContent, int requestContentLen, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int matches;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mRequestOrigin_Server;

    Lwm2m_Debug("WRITE (replace): %s\n", path);
    *responseContentLen = 0;

    // CoAP PUT is used for replace
    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);

    // Replace operation: must be O/I or O/I/R
    if ((oir[0] != -1) && (oir[1] != -1))
    {
        Lwm2mTreeNode * root;
        int len;

        // Create new resource instance with the values provided.
        len = Lwm2mCore_DeserialiseOIR(&root, contentType, context, oir, matches, requestContent, requestContentLen);

        if (len >= 0)
        {
            switch (Lwm2mTreeNode_GetType(root))
            {
                case Lwm2mTreeNodeType_Object:
                    Lwm2m_Error("Cannot replace a whole object using PUT\n");
                    *responseCode = Lwm2mResult_MethodNotAllowed;
                    break;
                case Lwm2mTreeNodeType_ObjectInstance:
                    if ((*responseCode = Lwm2mCore_CheckWritePermissionsForObjectInstanceNode(context, origin, root, oir[0], false)) == Lwm2mResult_Success)
                    {
                        if (Lwm2mCore_Exists(context, oir[0], oir[1], oir[2]) && Lwm2mCore_Delete(context, origin, oir[0], oir[1], oir[2]) == Lwm2mResult_SuccessDeleted)
                        {
                            *responseCode = Lwm2mCore_ParseObjectInstanceNodeAndWriteToStore(context, root, oir[0], true, true, false, &oir[1]);
                        }
                        else
                        {
                            *responseCode = Lwm2mResult_NotFound;
                        }
                    }
                    break;
                case Lwm2mTreeNodeType_Resource:
                    if ((*responseCode = Lwm2mCore_CheckWritePermissionsForResourceNode(context, origin, root, oir[0], oir[1], false)) == Lwm2mResult_Success)
                    {
                        // Unlike object instance, resource does not have to exist for PUT to succeed.
                        if (Lwm2mCore_Exists(context, oir[0], oir[1], oir[2]))
                        {
                            // Multiple-instance resources will be updated rather than replaced.
                            *responseCode = Lwm2mCore_ParseResourceNodeAndWriteToStore(context, root, oir[0], oir[1], true);
                        }
                        else
                        {
                            *responseCode = Lwm2mResult_NotFound;
                        }
                    }
                    break;
                case Lwm2mTreeNodeType_ResourceInstance: // no break
                default:
                    // Should never happen.
                    *responseCode = Lwm2mResult_InternalError;
                    break;
            }
        }
        else
        {
            *responseCode = Lwm2mResult_BadRequest;
        }
        Lwm2mTreeNode_DeleteRecursive(root);
    }
    else
    {
        *responseCode = Lwm2mResult_BadRequest;
    }
    return -1;
}

// Handle Write requests from the bootstrap server.
//
// The "Write" operation in Bootstrap interface is different from the "Write" operation in Device Managment and service enablement
// interface; The LWM2M Client MUST write the vlaue included in the payload regardless of an existence of the targeting object
// instance or resource. Only in bootstrap intercae, the "Write" MAY target just an Object ID, which will allow a Bootstrap server
// in using a TLV or JSON formatted payload, to populate a LWM2M Client in a single message containing serveral instances of the
// same object.
// Return 0 on success, non-zero on error.
static int Lwm2mCore_HandleBootstrapPutRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                               const char * requestContent, int requestContentLen, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int matches;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mRequestOrigin_BootstrapServer;
    int result = -1;

    Lwm2m_Debug("BOOTSTRAP WRITE: %s\n", path);

    *responseContentLen = 0;
    *responseCode = Lwm2mResult_BadRequest;

    matches = sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);
    Lwm2mTreeNode * root;

    // Create new resource instance with the values provided.
    int len = Lwm2mCore_DeserialiseOIR(&root, contentType, context, oir, matches, requestContent, requestContentLen);
    if (len >= 0)
    {
        switch (Lwm2mTreeNode_GetType(root))
        {
            case Lwm2mTreeNodeType_Object:
                if ((result = Lwm2mCore_CheckWritePermissionsForObjectNode(context, origin, root, false)) == Lwm2mResult_Success)
                {
                    *responseCode = Lwm2mCore_ParseObjectNodeAndWriteToStore(context, root, false, true, true, &oir[1]);
                }
                break;
            case Lwm2mTreeNodeType_ObjectInstance:
                if ((result = Lwm2mCore_CheckWritePermissionsForObjectInstanceNode(context, origin, root, oir[0], false)) == Lwm2mResult_Success)
                {
                    *responseCode = Lwm2mCore_ParseObjectInstanceNodeAndWriteToStore(context, root, oir[0], false, true, true, &oir[1]);
                }
                break;
            case Lwm2mTreeNodeType_Resource:
                if ((result = Lwm2mCore_CheckWritePermissionsForResourceNode(context, origin, root, oir[0], oir[1], false)) == Lwm2mResult_Success)
                {
                    *responseCode = Lwm2mCore_ParseResourceNodeAndWriteToStore(context, root, oir[0], oir[1], true);
                }
                break;
            case Lwm2mTreeNodeType_ResourceInstance: // no break
            default:
                // Should never happen.
                result = -1;
                break;
        }
    }
    Lwm2mTreeNode_DeleteRecursive(root);
    return result;
}

// Handle CoAP DELETE Requests, maps onto LWM2M DELETE operation. Return 0 on success, non-zero on error.
static int Lwm2mCore_HandleDeleteRequest(void * ctxt, AddressType * addr, const char * path, const char * query, ContentType contentType,
                                         const char * requestContent, int requestContentLen, char * responseContent, int * responseContentLen, int * responseCode)
{
    Lwm2mContextType * context = (Lwm2mContextType *)ctxt;
    int oir[3] = { -1, -1, -1 };
    Lwm2mRequestOrigin origin = Lwm2mCore_ServerIsBootstrap(context, addr) ? Lwm2mRequestOrigin_BootstrapServer : Lwm2mRequestOrigin_Server;

    *responseContentLen = 0;
    sscanf(path, "/%5d/%5d/%5d", &oir[0], &oir[1], &oir[2]);
    *responseCode = Lwm2mCore_Delete(context, origin, oir[0], oir[1], oir[2]);
    return 0;
}

// Handler for all "lwm2m" endpoints
static int Lwm2mCore_DeviceManagmentEndpointHandler(int type, void * ctxt, AddressType * addr,
                                                    const char * path, const char * query, const char * token, int tokenLength,
                                                    ContentType contentType, const char * requestContent, int requestContentLen,
                                                    ContentType * responseContentType, char * responseContent, int * responseContentLen, int * responseCode)
{
    switch (type)
    {
        case COAP_GET_REQUEST:
            return Lwm2mCore_HandleGetRequest(ctxt, addr, path, query, contentType,requestContent, requestContentLen, responseContentType, responseContent, responseContentLen, responseCode);

        case COAP_POST_REQUEST:
            return Lwm2mCore_HandlePostRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContent, responseContentLen, responseCode);

        case COAP_PUT_REQUEST:
            if (requestContentLen == 0 && strchr(query, '?') != NULL)
            {
                return Lwm2mCore_HandleWriteAttributesRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContent, responseContentLen, responseCode);
            }
            else if (Lwm2mCore_ServerIsBootstrap(ctxt, addr))
            {
                return Lwm2mCore_HandleBootstrapPutRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContent, responseContentLen, responseCode);
            }
            else
            {
                return Lwm2mCore_HandlePutRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContent, responseContentLen, responseCode);
            }

        case COAP_DELETE_REQUEST:
            return Lwm2mCore_HandleDeleteRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContent, responseContentLen, responseCode);

        case COAP_OBSERVE_REQUEST:
            return Lwm2mCore_HandleObserveRequest(ctxt, addr, path, query, (char*)token, tokenLength, contentType, requestContent, requestContentLen, responseContentType, responseContent, responseContentLen, responseCode);

        case COAP_CANCEL_OBSERVE_REQUEST:
            return Lwm2mCore_HandleCancelObserveRequest(ctxt, addr, path, query, contentType, requestContent, requestContentLen, responseContentType, responseContent, responseContentLen, responseCode);
    }

    *responseContentType = ContentType_None;
    *responseContentLen = 0;
    *responseCode = Lwm2mResult_MethodNotAllowed;
    return 0;
}

// This function is called by the CoAP library to handle any requests.
static int Lwm2mCore_HandleRequest(CoapRequest * request, CoapResponse * response)
{
    Lwm2mContextType * context = (Lwm2mContextType*)request->ctxt;

    // Look up the supplied path to determine what type of resource endpoint we are handling here.
    // There is a bit of a catch 22 here, because we need to lookup the endpoint type to determine how to lookup the endpoint
    // type. Therefore assume that GET requests are only ever valid for resources that exist, whereas POST/PUT/DELETE
    // operations can be performed as long as at least 1 ancestor exists.
    // There is an exception that DELETE has a special case for bootstrap delete on '/'
    ResourceEndPoint * endPoint;
    if ((request->type == COAP_GET_REQUEST) || ((request->type == COAP_DELETE_REQUEST) && (strcmp(request->path,"") == 0)))
    {
        endPoint = Lwm2mCore_FindResourceEndPoint(&context->EndPointList, request->path);
    }
    else
    {
        endPoint = Lwm2mCore_FindResourceEndPointAncestors(&context->EndPointList, request->path);
    }

    if (endPoint == NULL)
    {
        response->responseContentType = ContentType_None;
        response->responseContentLen = 0;
        response->responseCode = Lwm2mResult_NotFound;
        return 0;
    }

    return endPoint->Handler(request->type, request->ctxt, &request->addr, request->path, request->query,
                             request->token, request->tokenLength, request->contentType, request->requestContent,
                             request->requestContentLen, &response->responseContentType, response->responseContent,
                             &response->responseContentLen, &response->responseCode);
}

int Lwm2mCore_GetEndPointClientName(Lwm2mContextType * context, char * buffer, int len)
{
    // UUID URN: Identify a device using a Universally Unique Identifier (UUID).
    //           The UUID specifies a valid, hex digit character string as defined in [RFC4122].
    //           The format of the URN is urn:uuid:########-####-####-############
    // OPS URN: Identify a device using the format <OUI> "-" <ProductClass> "-" <SerialNumber> as defined in Section 3.4.4 of [TR-069].
    //           The format of the URN is urn:dev:ops:<OUI> "-" <ProductClass> "-" <SerialNumber>.
    // OS URN: Identify a device using the format <OUI> "-"<SerialNumber> as defined in Section 3.4.4 of [TR-069].
    //           The format of the URN is urn:dev:os:<OUI> "-"<SerialNumber>.
    // IMEI URN: Identify a device using an International Mobile Equipment IDentifiers [3GPP-TS_23.003].
    //            The IMEI URN specifies a valid, 15 digit IMEI. The format of the URN is urn:imei:###############
    // ESN URN: Identify a device using an Electronic Serial Number.
    //            The ESN specifies a valid, 8 digit ESN. The format of the URN is urn:esn:########
    // MEID URN: Identify a device using a Mobile Equipment IDentifier.
    //            The MEID URN specifies a valid, 14 digit
    //
    strcpy(buffer, context->EndPointName);
    return strlen(buffer);
}

// Update the LWM2M state machine, process any message timeouts, registeration attempts etc. Returns time until next service.
int Lwm2mCore_Process(Lwm2mContextType * context)
{
    int nextTick = 1000;

    // Update state machine.
    if ((context->BootStrapState == Lwm2mBootStrapState_BootStrapped) ||
        (context->BootStrapState == Lwm2mBootStrapState_CheckExisting))
    {
        // successfully bootstrapped, so we can now register with the servers.
        Lwm2m_UpdateRegistrationState(context);
    }

    if (context->BootStrapState != Lwm2mBootStrapState_BootStrapped)
    {
        // We haven't bootstrapped yet, so keep trying.
        Lwm2m_UpdateBootStrapState(context);
    }

    Lwm2m_UpdateObservers(context);
    return nextTick; 
}

// Initialise the LWM2M core, setup any callbacks, initialise CoAP etc. Returns the Context pointer.
Lwm2mContextType * Lwm2mCore_Init(CoapInfo * coap,  char * endPointName, const char * factoryBootstrapInformation)
{
    Lwm2mContextType * context = &Lwm2mContext;

    ListInit(&context->observerList);
    ListInit(&context->ServerList);
    Lwm2mObjectTree_Init(&context->ObjectTree);

    context->Coap = coap;
    context->Store = ObjectStore_Create();
    context->Definitions = DefinitionRegistry_Create();
    context->AttributeStore = AttributeStore_Create();
    Lwm2mSecurity_Create(&context->SecurityObjectList);

    Lwm2mCore_InitEndPointList(&context->EndPointList);

    if (endPointName != NULL)
    {
        strcpy(context->EndPointName, endPointName);
    }

    context->FactoryBootstrapInformation = factoryBootstrapInformation;

    if (coap != NULL)
    {
        coap_SetContext(context);
        coap_SetRequestHandler(Lwm2mCore_HandleRequest);
        Lwm2m_BootStrapInit(context);
    }

    return context;
}

// Called by the client on shutdown, de-register from server, clean up any resources
void Lwm2mCore_Destroy(Lwm2mContextType * context)
{
    Lwm2mObjectTree_Destroy(&context->ObjectTree);
    Lwm2mCore_DestroyEndPointList(&context->EndPointList);
    Lwm2mCore_DeregisterAllServers(context);
    Lwm2m_UpdateRegistrationState(context);

    Lwm2mCore_DestroyServerList(context);
    Lwm2mSecurity_Destroy(&context->SecurityObjectList);
    ObjectStore_Destroy(context->Store);
    AttributeStore_Destroy(context->AttributeStore);
    DefinitionRegistry_Destroy(context->Definitions);
    Lwm2m_FreeObservers(context);
}