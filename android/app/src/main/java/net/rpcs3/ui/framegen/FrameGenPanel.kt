package net.rpcs3.ui.framegen

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import net.rpcs3.R
import net.rpcs3.dialogs.AlertDialogQueue
import net.rpcs3.framegen.FrameGen
import net.rpcs3.framegen.FrameGenImportResult
import net.rpcs3.framegen.FrameGenMode
import net.rpcs3.framegen.FrameGenPrefs
import net.rpcs3.ui.components.GhostButton
import net.rpcs3.ui.components.SettingChip
import net.rpcs3.ui.components.SettingGroup
import net.rpcs3.ui.components.SettingSlider
import net.rpcs3.ui.components.SettingSwitch
import net.rpcs3.ui.components.SettingsHint
import net.rpcs3.ui.components.SettingsSection
import net.rpcs3.ui.components.ThinDivider
import net.rpcs3.ui.theme.Dimens
import net.rpcs3.ui.theme.Rpcs

const val FrameGenCategory = "Frame Gen"

private val TargetRates = listOf(60, 90, 120, 144)
private val Multipliers = listOf(2, 3, 4)

@Composable
fun FrameGenPanel(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val prefs = remember { FrameGenPrefs.of(context) }
    val scope = rememberCoroutineScope()
    val state by FrameGen.state

    var enabled by remember { mutableStateOf(FrameGenPrefs.isEnabled(prefs)) }
    var mode by remember { mutableStateOf(FrameGenPrefs.mode(prefs)) }
    var multiplier by remember { mutableIntStateOf(FrameGenPrefs.multiplier(prefs)) }
    var targetRate by remember { mutableIntStateOf(FrameGenPrefs.targetRate(prefs)) }
    var flowScale by remember { mutableIntStateOf(FrameGenPrefs.flowScale(prefs)) }
    var flowScaleAuto by remember { mutableStateOf(FrameGenPrefs.flowScaleAuto(prefs)) }
    var importing by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) { FrameGen.refresh(context) }

    val picker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
        onResult = { uri: Uri? ->
            if (uri != null) {
                importing = true
                scope.launch {
                    val result = withContext(Dispatchers.IO) { FrameGen.import(context, uri) }
                    importing = false

                    AlertDialogQueue.showDialog(
                        context.getString(R.string.framegen_import_title),
                        context.getString(result.messageRes)
                    )

                    if (result == FrameGenImportResult.Ok) {
                        enabled = FrameGenPrefs.isEnabled(prefs)
                    }
                }
            }
        }
    )

    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(Dimens.SectionGap)
    ) {
        SettingsSection(title = stringResource(R.string.framegen_section_source)) {
            SettingGroup {
                Text(
                    text = when {
                        importing -> stringResource(R.string.framegen_source_importing)
                        state.imported && state.sourceName.isNotEmpty() ->
                            stringResource(
                                R.string.framegen_source_loaded_named,
                                state.sourceName,
                                state.modules,
                                state.variant
                            )

                        state.imported ->
                            stringResource(
                                R.string.framegen_source_loaded,
                                state.modules,
                                state.variant
                            )

                        else -> stringResource(R.string.framegen_source_missing)
                    },
                    color = if (state.imported) Rpcs.TextPrimary else Rpcs.TextSecondary,
                    fontSize = Dimens.ValueSize,
                    fontWeight = if (state.imported) FontWeight.Medium else FontWeight.Normal,
                    lineHeight = 16.sp
                )
            }
        }

        SettingsSection(title = stringResource(R.string.framegen_section_generation)) {
            SettingGroup {
                SettingSwitch(
                    label = stringResource(R.string.framegen_enable),
                    subtitle = stringResource(
                        if (state.imported) {
                            R.string.framegen_enable_hint
                        } else {
                            R.string.framegen_enable_blocked
                        }
                    ),
                    checked = enabled && state.imported,
                    enabled = state.imported,
                    onCheckedChange = { wanted ->
                        enabled = wanted
                        FrameGenPrefs.setEnabled(prefs, wanted)
                        FrameGen.push(context)
                    }
                )

                ThinDivider()

                LabelledChipRow(label = stringResource(R.string.framegen_label_pacing)) {
                    FrameGenMode.entries.forEach { candidate ->
                        SettingChip(
                            label = stringResource(
                                if (candidate == FrameGenMode.Fixed) {
                                    R.string.framegen_mode_fixed
                                } else {
                                    R.string.framegen_mode_adaptive
                                }
                            ),
                            detail = stringResource(
                                if (candidate == FrameGenMode.Fixed) {
                                    R.string.framegen_mode_fixed_detail
                                } else {
                                    R.string.framegen_mode_adaptive_detail
                                }
                            ),
                            selected = candidate == mode,
                            enabled = state.imported,
                            modifier = Modifier.weight(1f),
                            onClick = {
                                mode = candidate
                                FrameGenPrefs.setMode(prefs, candidate)
                                FrameGen.push(context)
                            }
                        )
                    }
                }

                ThinDivider()

                if (mode == FrameGenMode.Fixed) {
                    LabelledChipRow(label = stringResource(R.string.framegen_label_multiplier)) {
                        Multipliers.forEach { candidate ->
                            SettingChip(
                                label = stringResource(
                                    R.string.framegen_multiplier_value,
                                    candidate
                                ),
                                detail = stringResource(
                                    R.string.framegen_multiplier_detail,
                                    candidate - 1
                                ),
                                selected = candidate == multiplier,
                                enabled = state.imported,
                                modifier = Modifier.weight(1f),
                                onClick = {
                                    multiplier = candidate
                                    FrameGenPrefs.setMultiplier(prefs, candidate)
                                    FrameGen.push(context)
                                }
                            )
                        }
                    }
                } else {
                    LabelledChipRow(label = stringResource(R.string.framegen_label_target)) {
                        TargetRates.forEach { candidate ->
                            SettingChip(
                                label = stringResource(R.string.framegen_target_value, candidate),
                                detail = stringResource(R.string.framegen_target_detail),
                                selected = candidate == targetRate,
                                enabled = state.imported,
                                modifier = Modifier.weight(1f),
                                onClick = {
                                    targetRate = candidate
                                    FrameGenPrefs.setTargetRate(prefs, candidate)
                                    FrameGen.push(context)
                                }
                            )
                        }
                    }
                }

                ThinDivider()

                LabelledChipRow(label = stringResource(R.string.framegen_label_flow)) {
                    SettingChip(
                        label = stringResource(R.string.framegen_flow_auto),
                        detail = stringResource(R.string.framegen_flow_auto_detail),
                        selected = flowScaleAuto,
                        enabled = state.imported,
                        modifier = Modifier.weight(1f),
                        onClick = {
                            flowScaleAuto = true
                            FrameGenPrefs.setFlowScaleAuto(prefs, true)
                            FrameGen.push(context)
                        }
                    )

                    SettingChip(
                        label = stringResource(R.string.framegen_flow_manual),
                        detail = stringResource(R.string.framegen_flow_manual_detail),
                        selected = !flowScaleAuto,
                        enabled = state.imported,
                        modifier = Modifier.weight(1f),
                        onClick = {
                            flowScaleAuto = false
                            FrameGenPrefs.setFlowScaleAuto(prefs, false)
                            FrameGen.push(context)
                        }
                    )
                }

                if (!flowScaleAuto) {
                    ThinDivider()

                    SettingSlider(
                        label = stringResource(R.string.framegen_label_flow_scale),
                        value = flowScale.toFloat(),
                        valueRange = 25f..100f,
                        steps = 14,
                        valueText = stringResource(R.string.percent_value, flowScale),
                        enabled = state.imported,
                        onValueChange = { flowScale = it.toInt() },
                        onValueChangeFinished = {
                            FrameGenPrefs.setFlowScale(prefs, flowScale)
                            FrameGen.push(context)
                        }
                    )
                }
            }

            SettingsHint(
                text = stringResource(
                    if (flowScaleAuto) R.string.framegen_flow_auto_hint else R.string.framegen_flow_hint
                )
            )
        }

        SettingsSection(title = stringResource(R.string.framegen_section_status)) {
            SettingGroup {
                Text(
                    text = when {
                        state.unsupported -> stringResource(R.string.framegen_runtime_unsupported)
                        state.ready -> stringResource(
                            R.string.framegen_runtime_active,
                            state.width,
                            state.height
                        )

                        else -> stringResource(R.string.framegen_runtime_idle)
                    },
                    color = when {
                        state.unsupported -> Rpcs.Warning
                        state.ready -> Rpcs.Success
                        else -> Rpcs.TextSecondary
                    },
                    fontSize = Dimens.ValueSize,
                    lineHeight = 16.sp
                )
            }

            SettingsHint(text = stringResource(R.string.framegen_latency_note))
        }

        SettingsSection(title = stringResource(R.string.framegen_section_manage)) {
            SettingsHint(text = stringResource(R.string.framegen_source_hint))

            Row(horizontalArrangement = Arrangement.spacedBy(Dimens.ItemGap)) {
                GhostButton(
                    label = stringResource(
                        if (state.imported) {
                            R.string.framegen_source_replace
                        } else {
                            R.string.framegen_source_select
                        }
                    ),
                    accent = true,
                    enabled = !importing,
                    modifier = Modifier.weight(1f),
                    onClick = { picker.launch(arrayOf("*/*")) }
                )

                if (state.imported) {
                    GhostButton(
                        label = stringResource(R.string.framegen_source_remove),
                        accent = true,
                        tint = Rpcs.Danger,
                        enabled = !importing,
                        modifier = Modifier.weight(1f),
                        onClick = {
                            scope.launch {
                                withContext(Dispatchers.IO) { FrameGen.forget(context) }
                                enabled = false
                                FrameGenPrefs.setEnabled(prefs, false)
                            }
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun LabelledChipRow(label: String, content: @Composable () -> Unit) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = label,
            color = Rpcs.TextSecondary,
            fontSize = Dimens.LabelSize,
            fontWeight = FontWeight.Medium
        )
        Spacer(Modifier.height(Dimens.TightGap))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(Dimens.TightGap)
        ) { content() }
    }
}
