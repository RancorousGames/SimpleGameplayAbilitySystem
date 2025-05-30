---
title: Attribute Modifiers
layout: home
parent: Blueprint Reference
nav_order: 3
---

# Attribute Modifiers
{: .no_toc }

<details close markdown="block">
<summary> Table of contents </summary>
{: .no_toc .text-delta }

1. TOC
{:toc}
</details>

## Overview
{: .no_toc }

Attribute Modifiers are specialized abilities that change attributes with sophisticated rules. They're the workhorses of SimpleGAS - the primary mechanism for modifying attributes during gameplay. Need to deal damage, apply a buff, or add a status effect? Attribute Modifiers are your go-to tool.

Attribute Modifiers come in two main types:
- **Instant Modifiers**: Apply their changes immediately and then end (like damage)
- **Duration Modifiers**: Persist for a period, potentially applying effects repeatedly (like damage over time)

Beyond simply changing values, Attribute Modifiers can:
- Chain multiple attribute modifications in a single operation
- Trigger side effects like visual effects, sounds, or animations
- Activate other abilities as side effects
- Apply or remove gameplay tags
- Cancel conflicting modifiers

<div class="api-docs" markdown="1">

## Properties

### General Configuration

| Name | Type | Description |
|:-----|:-----|:------------|
| Modifier Type | EAttributeModifierType | The behavior type: <br> - `Instant`: Apply modifications immediately and end <br> - `Duration`: Apply modifications over time |
| Modifier Application Policy | EAttributeModifierApplicationPolicy | Controls how the modifier is applied in multiplayer: <br> - `ApplyServerOnly`: Runs only on server with replicated results <br> - `ApplyServerOnlyButReplicateSideEffects`: Runs on server but side effects visible to clients <br> - `ApplyClientPredicted`: Runs immediately on client, then verified by server |
| Modifier Tags | FGameplayTagContainer | Tags that categorize this modifier (e.g., "DamageOverTime", "StatusEffect") |

### Duration Configuration 
*(Only used when Modifier Type is Duration)*

| Name | Type | Description |
|:-----|:-----|:------------|
| Has Infinite Duration | bool | If true, the modifier will not expire until explicitly removed |
| Duration | float | How long the modifier persists (in seconds) if not infinite |
| Tick On Apply | bool | Whether to apply effects immediately when the modifier is first applied |
| Tick Interval | float | How often the modifier applies its effects (in seconds) |
| Tick Tag Requirement Behaviour | EDurationTickTagRequirementBehaviour | How to handle ticks when tag requirements aren't met: <br> - `SkipOnTagRequirementFailed`: Skip the tick but continue timer <br> - `PauseOnTagRequirementFailed`: Pause timer until requirements are met again <br> - `CancelOnTagRequirementFailed`: End the modifier entirely |

### Stacking Configuration 
*(Only used when Modifier Type is Duration and CanStack is true)*

| Name | Type | Description |
|:-----|:-----|:------------|
| Can Stack | bool | Whether multiple instances combine (e.g., stacking poison) |
| Stacks | int32 | Initial stack count (usually 1) |
| Has Max Stacks | bool | Whether there's a limit to how many stacks can be applied |
| Max Stacks | int32 | Maximum allowed stacks if Has Max Stacks is true |

### Tag Requirements

| Name | Type | Description |
|:-----|:-----|:------------|
| Target Required Tags | FGameplayTagContainer | Tags that must be present on the target for the modifier to apply |
| Target Blocking Tags | FGameplayTagContainer | Tags that prevent the modifier from applying if present on the target |
| Target Blocking Modifier Tags | FGameplayTagContainer | If another modifier with these tags is already applied, this modifier won't apply |

### After passing tag requirements

| Name | Type | Description |
|:-----|:-----|:------------|
| Cancel Abilities | TArray&lt;TSubclassOf&lt;USimpleGameplayAbility&gt;&gt; | Ability classes to cancel when this modifier is applied |
| Cancel Abilities With Ability Tags | FGameplayTagContainer | Cancel abilities with any of these tags when this modifier is applied |
| Cancel Modifiers With Tag | FGameplayTagContainer | Cancel other modifiers with these tags when this modifier is applied |
| Temporarily Applied Tags | FGameplayTagContainer | Tags added to the target when this modifier is active and removed when it ends |
| Permanently Applied Tags | FGameplayTagContainer | Tags added to the target when this modifier is applied (not automatically removed) |
| Remove Gameplay Tags | FGameplayTagContainer | Tags to remove from the target when this modifier is applied |

### Attribute Modifications

This section defines how the modifier changes attributes. You can [modify both float and struct attributes](../../concepts/attributes/attributes.html#attributes).

| Name | Type | Description |
|:-----|:-----|:------------|
| Float Attribute Modifications | TArray&lt;FFloatAttributeModifier&gt; | Configuration for modifying float attributes |
| Struct Attribute Modifications | TArray&lt;FStructAttributeModifier&gt; | Configuration for modifying struct attributes |

### Side Effects

| Name | Type | Description |
|:-----|:-----|:------------|
| Ability Side Effects | TArray&lt;FAbilitySideEffect&gt; | Abilities to activate as side effects |
| Event Side Effects | TArray&lt;FEventSideEffect&gt; | Events to send as side effects |
| Attribute Modifier Side Effects | TArray&lt;FAttributeModifierSideEffect&gt; | Additional attribute modifiers to apply as side effects |

## Blueprint Implementable Events

### CanApplyModifier

Override this function to add custom validation rules for when the modifier can be applied.

**Parameters:**

| Input | Type | Description |
|:-------------|:------------------|:------|
| ModifierContext | FInstancedStruct | Context data passed during modifier application |

| Output | Type | Description |
|:-------------|:------------------|:------|
| Return Value | bool | True if the modifier can be applied, false otherwise |

### OnPreApplyModifier

Called after passing tag requirements and `CanApplyModifier` checks but before the modifier's effects are applied. Use this for setup or pre-application logic e.g. Applying resource costs.

**Parameters:**
*No parameters*

### OnPostApplyModifier

Called immediately after the modifier's effects are applied. Use for post-application logic or effects.

**Parameters:**
*No parameters*

### OnModifierEnded

Called when the modifier ends, either by completing its duration or being cancelled.

**Parameters:**

| Input | Type | Description |
|:-------------|:------------------|:------|
| EndingStatus | FGameplayTag | Tag indicating how the modifier ended |
| EndingContext | FInstancedStruct | Context data related to how the modifier ended |

### OnStacksAdded

Called when stacks are added to a duration modifier.

**Parameters:**

| Input | Type | Description |
|:-------------|:------------------|:------|
| AddedStacks | int32 | Number of stacks that were just added |
| CurrentStacks | int32 | Total number of stacks after addition |

### OnMaxStacksReached

Called when a duration modifier reaches its maximum stack count. This will only be called if `HasMaxStacks` is true.

**Parameters:**
*No parameters*

## How Attribute Modification Works

### Float Attribute Modifiers

Each float attribute modifier specifies:
1. Which attribute to modify (by tag)
2. What part of the attribute to change (current value, base value, etc.)
3. The operation to perform (add, subtract, multiply, etc.)
4. Where to get the input value from (fixed value, another attribute, overflow, etc.)
5. How to handle attribute limits (min/max values)

Operations include:
- Add: Output = A + B
- Subtract: Output = A - B
- Multiply: Output = A * B
- Divide: Output = A / B
- Power: Output = A^B
- Override: Output = B
- Custom: Call a blueprint function to calculate the output

### Struct Attribute Modifiers

Struct attribute modifiers work by calling a blueprint function you define. This gives you complete control over how to modify complex data structures. If you've set up a [`StructAttributeHandler`](../../concepts/attributes/attributes.html#struct-attribute-handlers), you will get events for each member of the struct that gets changed.


</div>