sys.path.append('./resources/libraries')
import os
import tensorflow as tf
from tensorflow.keras.optimizers.legacy import Adam
from tensorflow.keras.applications import MobileNetV2
from tensorflow.keras.layers import BatchNormalization, Conv2D, Softmax, Reshape
from tensorflow.keras.models import Model
from ei_tensorflow.constrained_object_detection import models, dataset, metrics, util
from ei_tensorflow.velo import train_keras_model_with_velo
from ei_shared.pretrained_weights import get_or_download_pretrained_weights
import ei_tensorflow.training

WEIGHTS_PREFIX = os.environ.get('WEIGHTS_PREFIX', os.getcwd())

def build_model(input_shape: tuple, weights: str, alpha: float,
                num_classes: int) -> tf.keras.Model:
    """ Modèle FOMO non compilé : entrée (B, H, W, C), sortie
    (B, H//8, W//8, num_classes) de logits. """

    mobile_net_v2 = MobileNetV2(input_shape=input_shape,
                                weights=weights,
                                alpha=alpha,
                                include_top=True)
    # Batch norm accéléré : le réglage par défaut vise de très gros réseaux.
    for layer in mobile_net_v2.layers:
        if type(layer) == BatchNormalization:
            layer.momentum = 0.9
    # MobileNet coupé là où il atteint 1/8 de la résolution d'entrée.
    cut_point = mobile_net_v2.get_layer('block_6_expand_relu')
    # Tête de détection ajoutée sur cette sortie intermédiaire.
    model = Conv2D(filters=32, kernel_size=1, strides=1,
                activation='relu', name='head')(cut_point.output)
    logits = Conv2D(filters=num_classes, kernel_size=1, strides=1,
                    activation=None, name='logits')(model)
    return Model(inputs=mobile_net_v2.input, outputs=logits)

def train(num_classes: int, learning_rate: float, num_epochs: int,
          alpha: float, object_weight: float,
          train_dataset: tf.data.Dataset,
          validation_dataset: tf.data.Dataset,
          best_model_path: str,
          input_shape: tuple,
          batch_size: int,
          use_velo: bool = False,
          ensure_determinism: bool = False) -> tf.keras.Model:
    """ Entraîne un modèle FOMO à num_classes + 1 sorties, la classe 0 étant
    le fond implicite. Les jeux de données (x, (bbox, one_hot_y)) sont
    convertis en (x, segmentation_map) et la perte est une entropie croisée
    pondérée par object_weight, le fond ayant un poids de 1.0. """

    nonlocal callbacks

    num_classes_with_background = num_classes + 1

    width, height, input_num_channels = input_shape
    if width != height:
        raise Exception(f"Only square inputs are supported; not {input_shape}")

    # Poids pré-entraînés, s'ils existent pour cette combinaison.
    allowed_combinations = [{'num_channels': 1, 'alpha': 0.1},
                            {'num_channels': 1, 'alpha': 0.35},
                            {'num_channels': 3, 'alpha': 0.1},
                            {'num_channels': 3, 'alpha': 0.35}]
    weights = get_or_download_pretrained_weights(WEIGHTS_PREFIX, input_num_channels, alpha, allowed_combinations)

    model = build_model(
        input_shape=input_shape,
        weights=weights,
        alpha=alpha,
        num_classes=num_classes_with_background
    )

    model_output_shape = model.layers[-1].output.shape
    _batch, width, height, num_classes = model_output_shape
    if width != height:
        raise Exception(f"Only square outputs are supported; not {model_output_shape}")
    output_width_height = width

    # Perte pondérée, dimensionnée pour la sortie de ce modèle.
    weighted_xent = models.construct_weighted_xent_fn(model.output.shape, object_weight)

    prefetch_policy = 1 if ensure_determinism else tf.data.experimental.AUTOTUNE

    # Les boîtes annotées deviennent des cartes de segmentation.
    def as_segmentation(ds, shuffle):
        ds = ds.map(dataset.bbox_to_segmentation(output_width_height, num_classes_with_background))
        if not ensure_determinism and shuffle:
            ds = ds.shuffle(buffer_size=batch_size*4)
        ds = ds.batch(batch_size, drop_remainder=False).prefetch(prefetch_policy)
        return ds

    train_segmentation_dataset = as_segmentation(train_dataset, True)
    validation_segmentation_dataset = as_segmentation(validation_dataset, False)

    validation_dataset_for_callback = (validation_dataset
        .batch(batch_size, drop_remainder=False)
        .prefetch(prefetch_policy))

    # Biais du classifieur final initialisé sur la distribution des données.
    util.set_classifier_biases_from_dataset(
        model, train_segmentation_dataset)

    if not use_velo:
        model.compile(loss=weighted_xent,
                    optimizer=Adam(learning_rate=learning_rate))

    # Score des centroïdes sur la validation à chaque fin d'époque.
    callbacks = callbacks if callbacks else []
    callbacks.append(metrics.CentroidScoring(validation_dataset_for_callback,
                                             output_width_height, num_classes_with_background))
    callbacks.append(metrics.PrintPercentageTrained(num_epochs))

    # Sauvegarde du meilleur modèle au sens du F1 de validation.
    callbacks.append(
        tf.keras.callbacks.ModelCheckpoint(best_model_path,
            monitor='val_f1', save_best_only=True, mode='max',
            save_weights_only=True, verbose=0))

    if use_velo:
        from tensorflow.python.framework.errors_impl import ResourceExhaustedError
        try:
            train_keras_model_with_velo(
                model,
                train_segmentation_dataset,
                validation_segmentation_dataset,
                loss_fn=weighted_xent,
                num_epochs=num_epochs,
                callbacks=callbacks
            )
        except ResourceExhaustedError as e:
            print(str(e))
            raise Exception(
                "ResourceExhaustedError caught during train_keras_model_with_velo."
                " Though VeLO encourages a large batch size, the current"
                f" size of {batch_size} may be too large. Please try a lower"
                " value. For further assistance please contact support"
                " at https://forum.edgeimpulse.com/")
    else:
        model.fit(train_segmentation_dataset,
                validation_data=validation_segmentation_dataset,
                epochs=num_epochs, callbacks=callbacks, verbose=0)

    model.load_weights(best_model_path)

    # Softmax explicite avant l'export.
    softmax_layer = Softmax()(model.layers[-1].output)
    model = Model(model.input, softmax_layer)

    return model


EPOCHS = args.epochs or 100
LEARNING_RATE = args.learning_rate or 0.001
BATCH_SIZE = args.batch_size or 32

# alpha et object_weight sont fixés ici, et non lus depuis l'interface du
# Studio : les modifier depuis le Studio n'a aucun effet en mode expert.
model = train(num_classes=classes,
              learning_rate=LEARNING_RATE,
              num_epochs=EPOCHS,
              alpha=0.35,
              object_weight=250,
              train_dataset=train_dataset,
              validation_dataset=validation_dataset,
              best_model_path=BEST_MODEL_PATH,
              input_shape=MODEL_INPUT_SHAPE,
              batch_size=BATCH_SIZE,
              use_velo=False,
              ensure_determinism=ensure_determinism)



disable_per_channel_quantization = False
