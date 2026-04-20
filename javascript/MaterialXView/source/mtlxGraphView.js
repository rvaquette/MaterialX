//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

import { LiteGraph } from 'litegraph.js';

const IGNORED_TAGS = new Set([
    'input',
    'output',
    'look',
    'materialassign',
    'collection',
    'propertyset',
    'propertysetassign',
    'geominfo',
    'token',
    'typedef',
    'nodedef',
    'implementation'
]);

export class MtlxGraphView
{
    constructor(graphCanvasId)
    {
        this._graphCanvasId = graphCanvasId;
        this._graph = new LiteGraph.LGraph();
        this._canvas = new LiteGraph.LGraphCanvas(`#${graphCanvasId}`, this._graph);
        this._canvas.background_image = null;
        this._canvas.ds.scale = 0.85;
    }

    setVisible(visible)
    {
        const panel = document.getElementById('graph-panel');
        if (panel)
        {
            panel.style.display = visible ? 'block' : 'none';
        }

        if (visible)
        {
            this._graph.start();
            this.resize();
        }
        else
        {
            this._graph.stop();
        }
    }

    resize()
    {
        if (this._canvas)
        {
            this._canvas.resize();
            this._canvas.setDirty(true, true);
        }
    }

    async loadFromFile(loader, materialFilename)
    {
        if (!materialFilename)
        {
            this._clearGraph('No material selected');
            return;
        }

        const xml = await new Promise((resolve, reject) =>
        {
            loader.load(materialFilename, data => resolve(data), null, reject);
        });

        if (typeof xml !== 'string' || xml.length === 0)
        {
            this._clearGraph('Unable to read .mtlx source');
            return;
        }

        this.loadFromXml(xml, materialFilename);
    }

    loadFromXml(xmlString, caption = 'MaterialX')
    {
        this._graph.clear();

        const parser = new DOMParser();
        const xmlDoc = parser.parseFromString(xmlString, 'application/xml');
        const parseError = xmlDoc.querySelector('parsererror');
        if (parseError)
        {
            this._clearGraph('Invalid .mtlx XML');
            return;
        }

        const root = xmlDoc.documentElement;
        if (!root)
        {
            this._clearGraph('Empty .mtlx document');
            return;
        }

        const graphNodeMap = new Map();
        const graphInputMap = new Map();
        const pendingLinks = [];

        const nodeElements = this._collectNodeElements(root);
        if (!nodeElements.length)
        {
            this._clearGraph('No nodes found in .mtlx');
            return;
        }

        this._addDocumentTitleNode(caption);

        let column = 0;
        let row = 0;
        const rowSize = 6;
        for (const element of nodeElements)
        {
            const scope = this._getScopeName(element);
            const name = element.getAttribute('name') || element.tagName;
            const key = `${scope}/${name}`;

            const graphNode = LiteGraph.createNode('basic/watch');
            graphNode.title = `${element.tagName}:${name}`;
            graphNode.size = [220, 80];

            const inputs = Array.from(element.children).filter(child => child.tagName === 'input');
            const outputs = Array.from(element.children).filter(child => child.tagName === 'output');

            if (!inputs.length)
            {
                graphNode.addInput('in', 'any');
            }
            else
            {
                for (const input of inputs)
                {
                    graphNode.addInput(input.getAttribute('name') || 'input', 'any');
                }
            }

            if (!outputs.length)
            {
                graphNode.addOutput('out', 'any');
            }
            else
            {
                for (const output of outputs)
                {
                    graphNode.addOutput(output.getAttribute('name') || 'output', 'any');
                }
            }

            graphNode.pos = [80 + column * 270, 80 + row * 150];
            this._graph.add(graphNode);
            graphNodeMap.set(key, { node: graphNode, element, scope });

            this._collectPendingLinks(element, scope, key, inputs, pendingLinks, graphInputMap);

            column++;
            if (column >= rowSize)
            {
                column = 0;
                row++;
            }
        }

        for (const link of pendingLinks)
        {
            const source = this._findSourceNode(link, graphNodeMap, graphInputMap);
            const target = graphNodeMap.get(link.targetNodeKey);
            if (!source || !target)
            {
                continue;
            }

            const sourceSlot = this._findOutputSlot(source.node, link.sourceOutputName);
            const targetSlot = this._findInputSlot(target.node, link.targetInputName);
            if (sourceSlot < 0 || targetSlot < 0)
            {
                continue;
            }

            source.node.connect(sourceSlot, target.node, targetSlot);
        }

        this._graph.arrange(120);
        this._graph.start();
        this._canvas.setDirty(true, true);
    }

    _addDocumentTitleNode(caption)
    {
        const titleNode = LiteGraph.createNode('basic/string');
        titleNode.title = 'MaterialX Document';
        titleNode.properties.value = caption;
        titleNode.pos = [40, 20];
        titleNode.size = [320, 60];
        this._graph.add(titleNode);
    }

    _collectNodeElements(root)
    {
        const candidates = root.querySelectorAll('[name]');
        return Array.from(candidates).filter(element =>
        {
            if (IGNORED_TAGS.has(element.tagName))
            {
                return false;
            }

            const hasInputs = element.querySelector(':scope > input') != null;
            const hasOutputs = element.querySelector(':scope > output') != null;
            const isGraph = element.tagName === 'nodegraph';
            const hasType = element.hasAttribute('type');

            return hasInputs || hasOutputs || isGraph || hasType;
        });
    }

    _collectPendingLinks(element, scope, targetNodeKey, inputs, pendingLinks, graphInputMap)
    {
        for (const input of inputs)
        {
            const targetInputName = input.getAttribute('name') || 'input';
            const sourceNodeName = input.getAttribute('nodename');
            const sourceOutputName = input.getAttribute('output') || 'out';

            if (sourceNodeName)
            {
                pendingLinks.push({
                    sourceNodeName,
                    sourceOutputName,
                    sourceScope: scope,
                    targetNodeKey,
                    targetInputName
                });
                continue;
            }

            const interfaceName = input.getAttribute('interfacename');
            if (interfaceName)
            {
                const interfaceKey = `${scope}/__interface__${interfaceName}`;
                let graphInputNode = graphInputMap.get(interfaceKey);
                if (!graphInputNode)
                {
                    graphInputNode = LiteGraph.createNode('basic/const');
                    graphInputNode.title = `interface:${interfaceName}`;
                    graphInputNode.addOutput(interfaceName, 'any');
                    graphInputNode.pos = [20, 120 + graphInputMap.size * 90];
                    this._graph.add(graphInputNode);
                    graphInputMap.set(interfaceKey, { node: graphInputNode, outputName: interfaceName, scope });
                }

                pendingLinks.push({
                    sourceNodeName: `__interface__${interfaceName}`,
                    sourceOutputName: interfaceName,
                    sourceScope: scope,
                    targetNodeKey,
                    targetInputName,
                    isInterface: true
                });
            }
        }
    }

    _findSourceNode(link, graphNodeMap, graphInputMap)
    {
        if (link.isInterface)
        {
            return graphInputMap.get(`${link.sourceScope}/${link.sourceNodeName}`) || null;
        }

        const scoped = graphNodeMap.get(`${link.sourceScope}/${link.sourceNodeName}`);
        if (scoped)
        {
            return scoped;
        }

        return graphNodeMap.get(`root/${link.sourceNodeName}`) || null;
    }

    _findInputSlot(node, name)
    {
        if (!node.inputs || !node.inputs.length)
        {
            return -1;
        }

        let index = node.inputs.findIndex(slot => slot.name === name);
        if (index < 0)
        {
            index = 0;
        }
        return index;
    }

    _findOutputSlot(node, name)
    {
        if (!node.outputs || !node.outputs.length)
        {
            return -1;
        }

        let index = node.outputs.findIndex(slot => slot.name === name);
        if (index < 0)
        {
            index = 0;
        }
        return index;
    }

    _getScopeName(element)
    {
        let current = element.parentElement;
        while (current)
        {
            if (current.tagName === 'nodegraph')
            {
                return current.getAttribute('name') || 'root';
            }
            current = current.parentElement;
        }
        return 'root';
    }

    _clearGraph(message)
    {
        this._graph.clear();
        const infoNode = LiteGraph.createNode('basic/string');
        infoNode.title = 'MaterialX Graph';
        infoNode.properties.value = message;
        infoNode.pos = [40, 40];
        infoNode.size = [320, 60];
        this._graph.add(infoNode);
        this._graph.start();
        this._canvas.setDirty(true, true);
    }
}
