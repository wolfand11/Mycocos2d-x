/****************************************************************************
Copyright (c) 2010-2012 cocos2d-x.org
Copyright (c) 2008-2010 Ricardo Quesada
Copyright (c) 2011      Zynga Inc.

http://www.cocos2d-x.org

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/

#ifndef __CCSCENE_H__
#define __CCSCENE_H__

#include "base_nodes/CCNode.h"
#include "physics/CCPhysicsWorld.h"

NS_CC_BEGIN

/**
 * @addtogroup scene
 * @{
 */

/** @brief Scene is a subclass of Node that is used only as an abstract concept.

Scene and Node are almost identical with the difference that Scene has its
anchor point (by default) at the center of the screen.

For the moment Scene has no other logic than that, but in future releases it might have
additional logic.

It is a good practice to use a Scene as the parent of all your nodes.
*/
class CC_DLL Scene : public Node
{
public:
    /** creates a new Scene object */
    static Scene *create();
#ifdef CC_USE_PHYSICS
    static Scene *createWithPhysics();
#endif

    Scene();
    /**
     * @js NA
     * @lua NA
     */
    virtual ~Scene();
    
    bool init();
    
#ifdef CC_USE_PHYSICS
public:
    bool initWithPhysics();
    
    virtual void addChild(Node* child) override;
    virtual void addChild(Node* child, int zOrder) override;
    virtual void addChild(Node* child, int zOrder, int tag) override;
    
    /*
     * Update method will be called automatically every frame if "scheduleUpdate" is called, and the node is "live"
     */
    virtual void update(float delta) override;
    
    inline PhysicsWorld* getPhysicsWorld() { return _physicsWorld; }
    
protected:
    virtual void addChildToPhysicsWorld(Node* child);
    
protected:
    PhysicsWorld* _physicsWorld;
#endif // CC_USE_PHYSICS
    
    friend class Layer;
    friend class SpriteBatchNode;
};

// end of scene group
/// @}

NS_CC_END

#endif // __CCSCENE_H__
